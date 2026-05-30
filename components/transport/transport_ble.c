/* components/transport/transport_ble.c
 * BLE Transport Implementation (Bluedroid)
 * Handles advertisement, scan, GATT exchange for pairing discovery.
 * Core BLE logic delegates to ESP-IDF Bluedroid stack.
 */

#include "transport_ble.h"
#include "session_fsm.h"
#include "ui_manager.h"
#include "ceepew_security_utils.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <string.h>
#include <stdio.h>
#include <esp_timer.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "transport_ble";

#ifdef CONFIG_BT_ENABLED
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatts_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#include "esp_err.h"
#endif

#ifdef CONFIG_BT_ENABLED
static void transport_ble_format_mac(const uint8_t mac[6], char out[18]);
static bool s_discovery_restart_pending = false;
static void transport_ble_log_peer_snapshot(const char *prefix,
                                            const BlePeerRecord_t *peer)
{
    if (peer == NULL) {
        ESP_LOGI(TAG, "%s peer=<none>", prefix);
        return;
    }

    char mac[18];
    char name[17];
    transport_ble_format_mac(peer->peer_mac, mac);
    memcpy(name, peer->name, peer->name_len);
    name[peer->name_len] = '\0';

    ESP_LOGI(TAG, "%s peer=%s rssi=%d seen_at=%lu name=%s hits=%u",
             prefix, mac, (int)peer->rssi, (unsigned long)peer->seen_at,
             (peer->name_len > 0U) ? name : "<none>", (unsigned)g_ble_ctx.scan_hit_count);
}

static void transport_ble_log_state_snapshot(const char *prefix);

static void transport_ble_update_state_from_flags(void)
{
    if (g_ble_ctx.is_advertising && g_ble_ctx.is_scanning) {
        g_ble_ctx.state = BLE_ADVERTISING_AND_SCANNING;
    } else if (g_ble_ctx.is_advertising) {
        g_ble_ctx.state = BLE_ADVERTISING;
    } else if (g_ble_ctx.is_scanning) {
        g_ble_ctx.state = BLE_SCANNING;
    } else if (s_discovery_restart_pending) {
        g_ble_ctx.state = BLE_ADVERTISING_AND_SCANNING;
    } else {
        g_ble_ctx.state = BLE_IDLE;
    }
}

static const uint16_t BLE_SERVICE_UUID = 0xFFF0;
static const uint16_t BLE_CHAR_UUID = 0xFFF1;
static const uint16_t BLE_VERIFY_CHAR_UUID = 0xFFF2;
static const uint8_t s_adv_service_uuid16[2] = {
    (uint8_t)(BLE_SERVICE_UUID & 0xFFU),
    (uint8_t)((BLE_SERVICE_UUID >> 8U) & 0xFFU)
};
static const uint8_t s_adv_raw_data[] = {
    2U, 0x01U, (uint8_t)(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    3U, 0x03U, (uint8_t)(BLE_SERVICE_UUID & 0xFFU), (uint8_t)((BLE_SERVICE_UUID >> 8U) & 0xFFU),
    7U, 0x09U, 'C', 'E', 'E', 'P', 'E', 'W'
};
static const uint8_t s_scan_rsp_raw_data[] = {
    7U, 0x09U, 'C', 'E', 'E', 'P', 'E', 'W'
};
static esp_bt_uuid_t s_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = BLE_CHAR_UUID }
};
static esp_bt_uuid_t s_verify_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = BLE_VERIFY_CHAR_UUID }
};
static esp_bt_uuid_t s_service_uuid_filter = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = BLE_SERVICE_UUID }
};
static esp_gatt_srvc_id_t s_service_id = {
    .id = {
        .uuid = {
            .len = ESP_UUID_LEN_16,
            .uuid = { .uuid16 = BLE_SERVICE_UUID }
        },
        .inst_id = 0U,
    },
    .is_primary = true,
};
#endif

#ifdef CONFIG_BT_ENABLED
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gattc_event_handler(esp_gattc_cb_event_t event,
                                esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param);
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param);

static const char *transport_ble_state_name(BleState_t state);
#endif

#define CEEPEW_RESTART_DEBOUNCE_MS  2000U
#define CEEPEW_PREVERIFY_RETRY_WINDOW_MS 10000U
#define CEEPEW_VERIFICATION_TIMEOUT_MS 20000U   /* Timeout waiting for peer verification result */
#define CEEPEW_SCAN_STUCK_TIMEOUT_MS        15000U /* If scan_seen_count doesn't change, consider scan stuck */
static uint32_t s_last_restart_ms              = 0U;
static uint32_t s_scan_retry_after_ms          = 0U;
static uint32_t s_preverify_disconnect_deadline_ms = 0U;
static bool     s_scan_requested               = false;
static bool     s_adv_data_set                 = false;
static bool     s_scan_rsp_set                 = false;
static bool     s_adv_starting                 = false;
static bool     s_scan_start_failed            = false;
static uint32_t s_commitment_retry_after_ms    = 0U;
static uint8_t  s_commitment_retry_count       = 0U;
/* Scan activity watchdog state */
static uint32_t s_last_scan_seen_value         = 0U;
static uint32_t s_last_scan_seen_change_ms     = 0U;

/* Mutex protecting access to g_ble_ctx transient fields that are read/written
 * from BLE callback context and task contexts on different cores. */
static SemaphoreHandle_t s_ble_ctx_mutex = NULL;

static inline void ble_ctx_lock(void)
{
    if (s_ble_ctx_mutex != NULL) { xSemaphoreTake(s_ble_ctx_mutex, portMAX_DELAY); }
}

static inline void ble_ctx_unlock(void)
{
    if (s_ble_ctx_mutex != NULL) { xSemaphoreGive(s_ble_ctx_mutex); }
}

/* [FIX-1] Per-write retry state */
static uint8_t  s_write_attempt                = 0U;
#define CEEPEW_WRITE_MAX_ATTEMPTS  4U          /* 3 retries after first try */
#define CEEPEW_WRITE_RETRY_DELAY_MS 250U

/* [FIX-2] Responder watchdog: if connected but no commitment arrives in
 *  CEEPEW_RESPONDER_WATCHDOG_MS, restart discovery. */
#define CEEPEW_RESPONDER_WATCHDOG_MS  35000U
static uint32_t s_responder_connected_at_ms    = 0U;  /* 0 = watchdog disarmed */
static bool s_gattc_mtu_ready = false;

static void transport_ble_log_state_snapshot(const char *prefix)
{
    char mac[18];
    transport_ble_format_mac(g_ble_ctx.peer_mac, mac);
    ESP_LOGI(TAG, "%s: state=%s discovered=%u gattc=%u gatts=%u conn_id=%u gattc_char=%u local_len=%u peer_pending=%u commit_pending=%u s_write_attempt=%u s_commit_retry_count=%u responder_watchdog_armed=%s reconnects=%u verify_fail=%u",
             prefix,
             transport_ble_state_name(g_ble_ctx.state),
             g_ble_ctx.discovered ? 1U : 0U,
             g_ble_ctx.gattc_connected ? 1U : 0U,
             g_ble_ctx.gatts_connected ? 1U : 0U,
             (unsigned)g_ble_ctx.conn_id,
             (unsigned)g_ble_ctx.gattc_char_handle,
             (unsigned)g_ble_ctx.local_commitment_len,
             g_ble_ctx.peer_commitment_pending ? 1U : 0U,
             g_ble_ctx.commitment_write_pending ? 1U : 0U,
             (unsigned)s_write_attempt,
             (unsigned)s_commitment_retry_count,
             (s_responder_connected_at_ms != 0U) ? "yes" : "no",
             (unsigned)g_ble_ctx.reconnect_attempts,
             (unsigned)g_ble_ctx.verify_fail_count);
}

CeePewErr_t transport_ble_restart_discovery_session(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    if (s_last_restart_ms != 0U &&
        (uint32_t)(now_ms - s_last_restart_ms) < CEEPEW_RESTART_DEBOUNCE_MS) {
        ESP_LOGW(TAG, "restart_discovery: debounced (%lu ms since last restart)",
                 (unsigned long)(now_ms - s_last_restart_ms));
        return CEEPEW_OK;
    }
    s_last_restart_ms = now_ms;

    uint8_t local_device_id[CEEPEW_DEVICE_ID_BYTES] = {0U};
    CeePewErr_t err = session_get_device_id(local_device_id);
    if (err != CEEPEW_OK) {
        ESP_LOGW(TAG, "session_get_device_id failed during pairing reset: %d", (int)err);
        return err;
    }

    err = session_phase1_init(local_device_id);
    if (err != CEEPEW_OK) {
        ESP_LOGW(TAG, "session_restart_discovery failed during pairing reset: %d", (int)err);
        return err;
    }

    /* Ensure BLE stack is in a clean state: disconnect and re-arm advertising/scan
     * so discovery can proceed without requiring a reboot. Ignore errors; caller logs */
    s_discovery_restart_pending = true;
    (void)transport_ble_disconnect();
    transport_ble_clear_discovery_peer_state();
    s_scan_requested = false;
    s_scan_start_failed = false;
    s_scan_retry_after_ms = 0U;
    s_adv_data_set = false;
    s_scan_rsp_set = false;
    s_adv_starting = false;
    (void)transport_ble_start_advertising();
    (void)transport_ble_start_scan();
    s_discovery_restart_pending = false;
    (void)ui_manager_transition_to(UI_STATE_DISCOVERY);
    ESP_LOGI(TAG, "transport_ble: discovery restarted (adv + scan requested)");

    return CEEPEW_OK;
}


/* Design note: The BLE transport uses Bluedroid (ESP-IDF's built-in BLE stack).
   Phase 1 uses advertisements for discovery. Phase 2 uses GATT for commitment
   exchange. Once commitment is verified, the session moves to Phase 3 over
   ESP-NOW, and BLE connection closes. This keeps BLE usage minimal (no bulk
   data transfer) and reserves bandwidth for ESP-NOW's higher throughput. */

BleContext_t g_ble_ctx = {0};

static bool s_ble_initialised = false;
static bool s_scan_peer_dedupe_valid = false;
static uint8_t s_scan_peer_dedupe_mac[6U] = {0U};
static uint32_t s_scan_peer_dedupe_seen_ms = 0U;

void transport_ble_clear_discovery_peer_state(void)
{
    memset(g_ble_ctx.peer_mac, 0U, sizeof(g_ble_ctx.peer_mac));
    memset(g_ble_ctx.peer_name, 0U, sizeof(g_ble_ctx.peer_name));
    g_ble_ctx.peer_name_len       = 0U;
    g_ble_ctx.peer_addr_type      = BLE_ADDR_TYPE_PUBLIC;
    g_ble_ctx.peer_rssi           = 0;
    g_ble_ctx.peer_rssi_smooth_x8 = 0;
    g_ble_ctx.last_seen_ms        = 0U;
    g_ble_ctx.gatt_connected_since_ms = 0U;
    g_ble_ctx.accumulated_conn_ms = 0U;
    g_ble_ctx.scan_hit_count      = 0U;
    g_ble_ctx.scan_seen_count     = 0U;
    g_ble_ctx.adv_packet_count    = 0U;
    g_ble_ctx.discovered          = false;
    g_ble_ctx.commitment_verified = false;
    g_ble_ctx.handoff_ready       = false;
    g_ble_ctx.ready_for_chat      = false;
    g_ble_ctx.peer_ready_for_chat = false;
    g_ble_ctx.peer_ready_timestamp_ms = 0U;
    g_ble_ctx.local_commitment_len = 0U;
    g_ble_ctx.peer_commitment_len  = 0U;
    g_ble_ctx.peer_commitment_legacy = false;
    g_ble_ctx.pending_peer_commitment_len = 0U;
    g_ble_ctx.peer_commitment_pending = false;
    g_ble_ctx.peer_commitment_read_issued = false;
    ceepew_secure_zero(g_ble_ctx.pending_peer_commitment, (uint32_t)sizeof(g_ble_ctx.pending_peer_commitment));
    g_ble_ctx.reconnect_attempts   = 0U;
    g_ble_ctx.verify_fail_count    = 0U;
    g_ble_ctx.connecting           = false;
    g_ble_ctx.is_initiator_role    = false;
    g_ble_ctx.commitment_write_pending = false;
    s_commitment_retry_after_ms = 0U;
    s_commitment_retry_count = 0U;
    s_preverify_disconnect_deadline_ms = 0U;
    memset(&g_ble_ctx.peer_record, 0U, sizeof(g_ble_ctx.peer_record));
    s_scan_peer_dedupe_valid = false;
    memset(s_scan_peer_dedupe_mac, 0U, sizeof(s_scan_peer_dedupe_mac));
    s_scan_peer_dedupe_seen_ms = 0U;

    /* [FIX-1] Reset write retry counter */
    s_write_attempt             = 0U;
    /* [FIX-2] Disarm responder watchdog */
    s_responder_connected_at_ms = 0U;
    /* [FIX-3] Clear MTU-ready flag */
    s_gattc_mtu_ready = false;
    /* [FIX-4] Clear verification fields */
    g_ble_ctx.verification_result = CEEPEW_VERIFY_PENDING;
    g_ble_ctx.peer_verification_result = CEEPEW_VERIFY_PENDING;
    g_ble_ctx.peer_verification_pending = false;
    g_ble_ctx.verification_timeout_ms = 0U;
    g_ble_ctx.peer_commitment_read_issued = false;
}

/*
 * s_adv_starting — set when esp_ble_gap_start_advertising() is called,
 * cleared on ADV_START_COMPLETE_EVT (success or failure).
 *
 * Prevents the second config-complete callback (ADV or SCAN_RSP) from
 * issuing a duplicate start call while the first is already in flight.
 */
/* s_adv_starting and s_scan_start_failed are declared earlier; do not redeclare here. */
#define CEEPEW_SCAN_PEER_DEDUPE_WINDOW_MS 300U

static const char *transport_ble_state_name(BleState_t state)
{
    switch (state) {
        case BLE_IDLE: return "idle";
        case BLE_ADVERTISING: return "advertising";
        case BLE_SCANNING: return "scanning";
        case BLE_ADVERTISING_AND_SCANNING: return "adv+scan";
        case BLE_CONNECTED: return "connected";
        case BLE_PAIRING: return "pairing";
        case BLE_DONE: return "done";
        default: return "unknown";
    }
}

static bool transport_ble_peer_is_recent_at(uint32_t now_ms, uint32_t max_age_ms)
{
    CEEPEW_ASSERT(max_age_ms > 0U, false);

    if (!g_ble_ctx.discovered || g_ble_ctx.last_seen_ms == 0U) {
        return false;
    }

    return (uint32_t)(now_ms - g_ble_ctx.last_seen_ms) <= max_age_ms;
}

static uint8_t transport_ble_reason_from_gatt_status(esp_gatt_status_t status)
{
    if (status == ESP_GATT_AUTH_FAIL || status == ESP_GATT_INSUF_AUTHENTICATION) {
        return UI_PAIRING_RESULT_COMMITMENT_FAIL;
    }
    return UI_PAIRING_RESULT_LINK_FAIL;
}

static bool transport_ble_local_commitment_ready(void)
{
    if (g_ble_ctx.local_commitment_len != CEEPEW_COMMITMENT_BYTES &&
        g_ble_ctx.local_commitment_len != CEEPEW_COMMITMENT_LEGACY_BYTES) {
        return false;
    }

    uint8_t nonzero = 0U;
    for (uint8_t i = 0U; i < g_ble_ctx.local_commitment_len; i++) {
        nonzero |= g_ble_ctx.commitment_digest[i];
    }
    return nonzero != 0U;
}

static CeePewErr_t transport_ble_send_commitment_write(void)
{
#ifdef CONFIG_BT_ENABLED
    if (!g_ble_ctx.gattc_connected || g_ble_ctx.gattc_char_handle == 0U) {
        return CEEPEW_ERR_BUSY;
    }

    /* If MTU handshake not yet completed, let the retry path wait for CFG_MTU_EVT */
    if (!s_gattc_mtu_ready) {
        ESP_LOGI(TAG, "MTU not ready; deferring commitment write");
        return CEEPEW_ERR_BUSY;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (s_commitment_retry_after_ms != 0U && now_ms < s_commitment_retry_after_ms) {
        return CEEPEW_ERR_BUSY;
    }

    CEEPEW_ASSERT(transport_ble_local_commitment_ready(), CEEPEW_ERR_PARAM);
    esp_err_t err = esp_ble_gattc_write_char(
        g_ble_ctx.gattc_if,
        g_ble_ctx.conn_id,
        g_ble_ctx.gattc_char_handle,
        (uint16_t)g_ble_ctx.local_commitment_len,
        g_ble_ctx.commitment_digest,
        ESP_GATT_WRITE_TYPE_RSP,
        ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gattc_write_char failed: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "esp_ble_gattc_write_char queued (len=%u)", (unsigned)g_ble_ctx.local_commitment_len);
    return CEEPEW_OK;
#else
    return CEEPEW_OK;
#endif
}

static CeePewErr_t transport_ble_request_peer_commitment_read(void)
{
#ifdef CONFIG_BT_ENABLED
    if (!g_ble_ctx.gattc_connected || g_ble_ctx.gattc_verify_char_handle == 0U) {
        return CEEPEW_ERR_BUSY;
    }

    if (!g_ble_ctx.peer_verification_pending || g_ble_ctx.peer_commitment_read_issued) {
        return CEEPEW_OK;
    }

    esp_err_t err = esp_ble_gattc_read_char(
        g_ble_ctx.gattc_if,
        g_ble_ctx.conn_id,
        g_ble_ctx.gattc_verify_char_handle,
        ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gattc_read_char failed: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }

    g_ble_ctx.peer_commitment_read_issued = true;
    ESP_LOGI(TAG, "esp_ble_gattc_read_char queued for peer commitment");
    return CEEPEW_OK;
#else
    return CEEPEW_OK;
#endif
}

static bool transport_ble_scan_peer_is_duplicate(const uint8_t mac[6U],
                                                 uint32_t now_ms)
{
    CEEPEW_ASSERT(mac != NULL, false);

    if (!s_scan_peer_dedupe_valid) {
        memcpy(s_scan_peer_dedupe_mac, mac, 6U);
        s_scan_peer_dedupe_seen_ms = now_ms;
        s_scan_peer_dedupe_valid = true;
        return false;
    }

    if (memcmp(s_scan_peer_dedupe_mac, mac, 6U) != 0) {
        memcpy(s_scan_peer_dedupe_mac, mac, 6U);
        s_scan_peer_dedupe_seen_ms = now_ms;
        return false;
    }

    if ((uint32_t)(now_ms - s_scan_peer_dedupe_seen_ms) <= CEEPEW_SCAN_PEER_DEDUPE_WINDOW_MS) {
        return true;
    }

    s_scan_peer_dedupe_seen_ms = now_ms;
    return false;
}

static void transport_ble_format_mac(const uint8_t mac[6], char out[18])
{
    (void)snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool transport_ble_payload_contains(const uint8_t *buf,
                                           uint16_t buf_len,
                                           const uint8_t *needle,
                                           uint8_t needle_len)
{
    CEEPEW_ASSERT(buf != NULL, false);
    CEEPEW_ASSERT(needle != NULL && needle_len > 0U, false);

    if (buf_len < needle_len) {
        return false;
    }

    uint16_t limit = (uint16_t)(buf_len - needle_len + 1U);
    for (uint16_t i = 0U; i < limit; i++) {
        bool match = true;
        for (uint8_t j = 0U; j < needle_len; j++) {
            if (buf[i + j] != needle[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }

    return false;
}

CeePewErr_t transport_ble_init(void)
{
    CEEPEW_ASSERT(!s_ble_initialised, CEEPEW_ERR_BUSY);

    memset(&g_ble_ctx, 0U, sizeof(BleContext_t));
    /* Clear any transient peer info */
    memset(g_ble_ctx.peer_name, 0U, sizeof(g_ble_ctx.peer_name));
    g_ble_ctx.peer_name_len = 0U;
    g_ble_ctx.peer_rssi = 0;
    g_ble_ctx.peer_rssi_smooth_x8 = 0;
    g_ble_ctx.last_seen_ms = 0U;
    g_ble_ctx.gatt_connected_since_ms = 0U;
    g_ble_ctx.scan_hit_count      = 0U;
    g_ble_ctx.scan_seen_count = 0U;
    g_ble_ctx.adv_packet_count = 0U;  /* Initialize advertising counter */

    g_ble_ctx.state = BLE_IDLE;
    g_ble_ctx.discovery_start_ts = 0U;
    g_ble_ctx.pairing_start_ts = 0U;
    g_ble_ctx.commitment_verified = false;
    g_ble_ctx.handoff_ready = false;
    g_ble_ctx.is_advertising = false;
    g_ble_ctx.is_scanning = false;
    g_ble_ctx.gattc_if = CEEPEW_GATT_IF_NONE;
    g_ble_ctx.gatts_if = CEEPEW_GATT_IF_NONE;
    g_ble_ctx.conn_id = 0U;
    g_ble_ctx.service_start_handle = 0U;
    g_ble_ctx.service_end_handle = 0U;
    g_ble_ctx.gattc_char_handle = 0U;
    g_ble_ctx.gatts_char_handle = 0U;
    g_ble_ctx.gattc_registered = false;
    g_ble_ctx.gatts_registered = false;
    g_ble_ctx.gattc_connected = false;
    g_ble_ctx.gatts_connected = false;
    g_ble_ctx.connecting = false;
    g_ble_ctx.commitment_write_pending = false;
    g_ble_ctx.verification_result = CEEPEW_VERIFY_PENDING;
    g_ble_ctx.peer_verification_result = CEEPEW_VERIFY_PENDING;
    g_ble_ctx.peer_verification_pending = false;
    g_ble_ctx.verification_timeout_ms = 0U;
    g_ble_ctx.peer_addr_type = BLE_ADDR_TYPE_PUBLIC;
    g_ble_ctx.pending_peer_commitment_len = 0U;
    g_ble_ctx.peer_commitment_pending = false;
    memset(g_ble_ctx.pending_peer_commitment, 0U, sizeof(g_ble_ctx.pending_peer_commitment));
    s_scan_requested = false;
    s_adv_data_set = false;
    s_scan_rsp_set = false;
    s_scan_peer_dedupe_valid = false;
    s_scan_peer_dedupe_seen_ms = 0U;
    memset(s_scan_peer_dedupe_mac, 0U, sizeof(s_scan_peer_dedupe_mac));

    /* Ensure per-session transient state is cleared */
    s_write_attempt = 0U;
    s_responder_connected_at_ms = 0U;
    ESP_LOGI(TAG, "BLE context reset: state=%s adv=%u scan=%u",
             transport_ble_state_name(g_ble_ctx.state),
             g_ble_ctx.is_advertising ? 1U : 0U,
             g_ble_ctx.is_scanning ? 1U : 0U);

    /* Create mutex for protecting g_ble_ctx access across tasks/cores */
    s_ble_ctx_mutex = xSemaphoreCreateMutex();
    if (s_ble_ctx_mutex == NULL) {
        ESP_LOGW(TAG, "transport_ble_init: failed to create ble_ctx mutex");
    }

#ifdef CONFIG_BT_ENABLED
    ESP_LOGI(TAG, "transport_ble_init: Starting BLE initialization");

    esp_err_t err;
    /* Initialize BT controller, then bluedroid stack. Some platforms may have
       already initialized the controller; ignore invalid-state returns. */
    /* NOTE: esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT) was
     * called in main.c before hal_radio_init(). Do NOT call it here.
     */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bt_controller_init FAILED: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "esp_bt_controller_init: OK");

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bt_controller_enable FAILED: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "esp_bt_controller_enable: OK");

    /* Initialize bluedroid if available; some platforms already do controller init elsewhere */
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    err = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bluedroid_init_with_cfg FAILED: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "esp_bluedroid_init_with_cfg: OK");

    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bluedroid_enable FAILED: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "esp_bluedroid_enable: OK");

    /* Set initialization flag BEFORE registering callbacks to avoid race condition.
     * GAP callbacks may fire during esp_ble_gap_register_callback() or
     * esp_ble_gap_set_device_name(), and those callbacks need s_ble_initialised
     * to be true to pass assertions.
     */
    s_ble_initialised = true;

    /* Register BLE callbacks to receive events. Implementations are no-op but
       allow upper layers to rely on the event-driven flow. */
    esp_ble_gap_register_callback(gap_event_handler);
    ESP_LOGI(TAG, "GAP callback registered");

    esp_ble_gattc_register_callback(gattc_event_handler);
    ESP_LOGI(TAG, "GATTC callback registered");

    esp_ble_gatts_register_callback(gatts_event_handler);
    ESP_LOGI(TAG, "GATTS callback registered");

    /* Start advertising early, in parallel with GATT registration.
     * This ensures that by the time GATTC registers and scan starts,
     * the advertising is already on the air and will be discovered.
     * transport_ble_start_advertising() will configure and start BLE ads.
     */
    CeePewErr_t adv_err = transport_ble_start_advertising();
    if (adv_err != CEEPEW_OK) {
        ESP_LOGW(TAG, "Early advertising start returned: %d (will retry on GATT events)", adv_err);
    } else {
        ESP_LOGI(TAG, "Advertising configuration initiated early");
    }

    err = esp_ble_gattc_app_register(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gattc_app_register failed: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }

    err = esp_ble_gatts_app_register(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gatts_app_register failed: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }

    ESP_LOGI(TAG, "transport_ble_init: COMPLETE - BLE ready");
#endif

    return CEEPEW_OK;
}

CeePewErr_t transport_ble_start_advertising(void)
{
    CEEPEW_ASSERT(s_ble_initialised, CEEPEW_ERR_PARAM);
    /* Allow re-entry if already in discovery loop */
    if (g_ble_ctx.state != BLE_IDLE &&
        g_ble_ctx.state != BLE_ADVERTISING &&
        g_ble_ctx.state != BLE_SCANNING &&
        g_ble_ctx.state != BLE_ADVERTISING_AND_SCANNING) {
        return CEEPEW_ERR_BUSY;
    }
     /* Do not re-advertise once a session is active */
    if (session_is_active()) {
        ESP_LOGI(TAG, "start_advertising: session active — skipping re-advertise");
        return CEEPEW_OK;
    }

#ifdef CONFIG_BT_ENABLED
    /* Reset flags so the two-callback gate works correctly on every call */
    s_adv_data_set = false;
    s_scan_rsp_set = false;
    ESP_LOGI(TAG, "Advertising requested: state=%s peer_found=%u",
             transport_ble_state_name(g_ble_ctx.state),
             g_ble_ctx.discovered ? 1U : 0U);

    esp_err_t err;
    /* Set a device name for easier identification during scanning */
    err = esp_ble_gap_set_device_name("CEEPEW");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gap_set_device_name returned %d", err);
    } else {
        ESP_LOGI(TAG, "Device name set to 'CEEPEW'");
    }

    err = esp_ble_gap_config_adv_data_raw((uint8_t *)s_adv_raw_data,
                                          sizeof(s_adv_raw_data));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_config_adv_data_raw FAILED: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }

    /* Configure scan response separately so active scanners always receive name */
    err = esp_ble_gap_config_scan_rsp_data_raw((uint8_t *)s_scan_rsp_raw_data,
                                               sizeof(s_scan_rsp_raw_data));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_config_scan_rsp_data_raw FAILED: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }

    ESP_LOGI(TAG, "Advertisement and scan response configured; awaiting GAP callbacks (svc=0x%04X)",
             (unsigned)BLE_SERVICE_UUID);

#else
    /* Bluetooth not enabled in sdkconfig; we still update state for higher-level logic */
#endif

    g_ble_ctx.is_advertising = false;  /* confirmed via ADV_START_COMPLETE_EVT */
    if (g_ble_ctx.is_advertising && g_ble_ctx.is_scanning) {
        g_ble_ctx.state = BLE_ADVERTISING_AND_SCANNING;
    } else if (g_ble_ctx.is_advertising) {
        g_ble_ctx.state = BLE_ADVERTISING;
    } else if (g_ble_ctx.is_scanning) {
        g_ble_ctx.state = BLE_SCANNING;
    } else {
        g_ble_ctx.state = BLE_IDLE;
    }
    g_ble_ctx.discovery_start_ts = (uint32_t)(esp_timer_get_time() / 1000000LL);
    ESP_LOGI(TAG, "Advertising armed: state=%s discovery_start=%lu",
             transport_ble_state_name(g_ble_ctx.state),
             (unsigned long)g_ble_ctx.discovery_start_ts);

    return CEEPEW_OK;
}

CeePewErr_t transport_ble_start_scan(void)
{
    CEEPEW_ASSERT(s_ble_initialised, CEEPEW_ERR_PARAM);

    if (s_scan_requested) {
        ESP_LOGI(TAG, "Scan already requested; skipping duplicate start");
        return CEEPEW_OK;
    }

    if (g_ble_ctx.is_scanning ||
        g_ble_ctx.state == BLE_SCANNING ||
        g_ble_ctx.state == BLE_ADVERTISING_AND_SCANNING) {
        ESP_LOGI(TAG, "Scan already active; clearing retry state");
        s_scan_requested = false;
        s_scan_start_failed = false;
        s_scan_retry_after_ms = 0U;
        return CEEPEW_OK;
    }

    if (g_ble_ctx.state != BLE_IDLE &&
        g_ble_ctx.state != BLE_ADVERTISING &&
        g_ble_ctx.state != BLE_SCANNING &&
        g_ble_ctx.state != BLE_ADVERTISING_AND_SCANNING) {
        return CEEPEW_ERR_BUSY;
    }

#ifdef CONFIG_BT_ENABLED
    /* Configure scan parameters before starting. Without this the BLE stack
     * uses whatever defaults are in silicon — on ESP32 these may give a scan
     * window so narrow that most advertisements are missed entirely.
     *
     * ACTIVE scan: the radio sends scan-request packets, which causes remote
     * devices to reply with scan-response packets. That is how we receive the
     * full "CEEPEW" name (it may be in the scan-response, not the ADV_IND).
     * PASSIVE scan would silently drop the scan-response and we'd see no name.
     */
    ESP_LOGI(TAG, "Scan requested: active=%u interval=%u window=%u duplicate=%u",
             1U, 0x50U, 0x30U, 0U);
    esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x50,   /* 50ms = 80 × 0.625ms */
        .scan_window        = 0x30,   /* 30ms — leaves room for TX */
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,  /* keep live RSSI updates */
    };

    esp_err_t err = esp_ble_gap_set_scan_params(&scan_params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_set_scan_params failed: %d (%s)",
                 err, esp_err_to_name(err));
        s_scan_requested = false;
        s_scan_start_failed = true;
        s_scan_retry_after_ms = (uint32_t)(esp_timer_get_time() / 1000LL) +
                                CEEPEW_RESTART_DEBOUNCE_MS;
        return CEEPEW_ERR_HW;
    }
    s_scan_requested = true;
    /* Scanning starts inside ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT below. */
    ESP_LOGI(TAG, "Scan parameters set — waiting for SCAN_PARAM_SET_COMPLETE_EVT");
#endif

    /* FIX: Do NOT touch g_ble_ctx.state or g_ble_ctx.is_scanning here.
     * The state is updated in ESP_GAP_BLE_SCAN_START_COMPLETE_EVT via
     * transport_ble_update_state_from_flags(). Setting it to BLE_IDLE
     * here caused the pairing driver to see an idle bus and stall.
     */
    ESP_LOGI(TAG, "Scan params sent — waiting for SCAN_PARAM_SET_COMPLETE_EVT (state=%s)",
             transport_ble_state_name(g_ble_ctx.state));
    return CEEPEW_OK;
}

BleState_t transport_ble_get_state(void)
{
    return g_ble_ctx.state;
}

const BlePeerRecord_t *transport_ble_get_peer(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (!transport_ble_peer_is_recent_at(now_ms, CEEPEW_DISCOVERY_PEER_VISIBLE_MS)) {
        return NULL;
    }
    return &g_ble_ctx.peer_record;
}

const BlePeerRecord_t *transport_ble_get_peer_cached(void)
{
    if (!g_ble_ctx.discovered) {
        return NULL;
    }
    return &g_ble_ctx.peer_record;
}

bool transport_ble_has_peer_cached(void)
{
    return transport_ble_get_peer_cached() != NULL;
}

CeePewErr_t transport_ble_connect_to_peer(const uint8_t peer_mac[6])
{
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);

    if (g_ble_ctx.gattc_connected || g_ble_ctx.connecting) {
        return CEEPEW_ERR_BUSY;
    }

    memcpy(g_ble_ctx.peer_mac, peer_mac, 6U);
    char peer_mac_str[18];
    transport_ble_format_mac(peer_mac, peer_mac_str);
    ESP_LOGI(TAG, "Connect requested: peer=%s addr_type=%u state=%s",
             peer_mac_str, (unsigned)g_ble_ctx.peer_addr_type,
             transport_ble_state_name(g_ble_ctx.state));

#ifdef CONFIG_BT_ENABLED
    if (g_ble_ctx.gattc_if == CEEPEW_GATT_IF_NONE || !g_ble_ctx.gattc_registered) {
        ESP_LOGE(TAG, "gattc_if not registered yet");
        return CEEPEW_ERR_BUSY;
    }
    g_ble_ctx.connecting = true;
    g_ble_ctx.is_initiator_role = true;  /* We are opening the GATTC connection */
    esp_ble_gatt_creat_conn_params_t conn_params = {0};
    memcpy(conn_params.remote_bda, g_ble_ctx.peer_mac, ESP_BD_ADDR_LEN);
    conn_params.remote_addr_type = g_ble_ctx.peer_addr_type;
    conn_params.own_addr_type    = BLE_ADDR_TYPE_PUBLIC;
    conn_params.is_direct        = true;
    conn_params.is_aux           = false;
    conn_params.phy_mask         = 0x0;
    esp_err_t err = esp_ble_gattc_enh_open(g_ble_ctx.gattc_if, &conn_params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gattc_open/enh_open failed: %d", err);
        g_ble_ctx.connecting = false;
        return CEEPEW_ERR_HW;
    }
    /* Actual state transition will occur in GATTC callback when connected */
#else
    /* If BT disabled, simulate immediate connection for higher-level logic */
    g_ble_ctx.gattc_connected = true;
    g_ble_ctx.is_initiator_role = true;
    g_ble_ctx.state = BLE_CONNECTED;
#endif

    return CEEPEW_OK;
}

CeePewErr_t transport_ble_exchange_commitment(const uint8_t *commitment_digest, uint8_t len)
{
    CEEPEW_ASSERT(commitment_digest != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(g_ble_ctx.state == BLE_CONNECTED, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(len == CEEPEW_COMMITMENT_BYTES || len == CEEPEW_COMMITMENT_LEGACY_BYTES, CEEPEW_ERR_PARAM);

    /* Build local commitment+signature using the session layer (always prefer signed form when available). */
    uint8_t signed_commit[CEEPEW_COMMITMENT_BYTES + 64U];
    uint8_t signed_len = 0U;
    CeePewErr_t serr = session_get_commitment_with_sig(signed_commit, &signed_len);
    if (serr != CEEPEW_OK) {
        /* Fallback: if session layer cannot produce signed commitment, fall back to caller-provided digest */
        for (uint8_t i = 0U; i < (CEEPEW_COMMITMENT_BYTES + 64U); i++) { g_ble_ctx.commitment_digest[i] = 0U; }
        memcpy(g_ble_ctx.commitment_digest, commitment_digest, len);
        g_ble_ctx.local_commitment_len = len;
    } else {
        for (uint8_t i = 0U; i < (CEEPEW_COMMITMENT_BYTES + 64U); i++) { g_ble_ctx.commitment_digest[i] = 0U; }
        memcpy(g_ble_ctx.commitment_digest, signed_commit, signed_len);
        g_ble_ctx.local_commitment_len = signed_len;
    }

#ifdef CONFIG_BT_ENABLED
    /* Publish local commitment to our GATTS verify characteristic (so peers can read it).
     * This avoids requiring the responder to open a client connection back to write a
     * verification result. */
    if (g_ble_ctx.gatts_registered && g_ble_ctx.gatts_verify_char_handle != 0U) {
        esp_err_t set_err = esp_ble_gatts_set_attr_value(
            g_ble_ctx.gatts_verify_char_handle,
            (uint16_t)g_ble_ctx.local_commitment_len,
            g_ble_ctx.commitment_digest);
        if (set_err == ESP_OK) {
            ESP_LOGI(TAG, "Published local commitment to GATTS verify characteristic");
        } else {
            ESP_LOGW(TAG, "Failed to publish local commitment to GATTS: %d", set_err);
        }
    }
#endif

    ESP_LOGI(TAG, "Commitment exchange queued: state=%s connected=%u char_handle=%u len=%u",
             transport_ble_state_name(g_ble_ctx.state),
             g_ble_ctx.gattc_connected ? 1U : 0U,
             (unsigned)g_ble_ctx.gattc_char_handle, (unsigned)len);

#ifdef CONFIG_BT_ENABLED
    g_ble_ctx.peer_verification_pending = true;
    g_ble_ctx.peer_verification_result = CEEPEW_VERIFY_PENDING;
    g_ble_ctx.peer_commitment_read_issued = false;
    g_ble_ctx.verification_timeout_ms =
        (uint32_t)(esp_timer_get_time() / 1000ULL) + CEEPEW_VERIFICATION_TIMEOUT_MS;

    if (g_ble_ctx.gattc_connected && g_ble_ctx.gattc_char_handle != 0U) {
        CeePewErr_t write_err = transport_ble_send_commitment_write();
        if (write_err != CEEPEW_OK) {
            g_ble_ctx.commitment_write_pending = true;
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            uint32_t retry_after_ms = now_ms + 400U;
            if (s_commitment_retry_after_ms < retry_after_ms) {
                s_commitment_retry_after_ms = retry_after_ms;
            }
        } else {
            g_ble_ctx.commitment_write_pending = false;
            s_commitment_retry_after_ms = 0U;
            s_commitment_retry_count = 0U;
        }
    } else {
        g_ble_ctx.commitment_write_pending = true;
        s_commitment_retry_after_ms = (uint32_t)(esp_timer_get_time() / 1000ULL) + 400U;
    }

    (void)transport_ble_request_peer_commitment_read();
#endif

    /* FIX A2: If a peer commitment arrived before our local commitment was
     * ready (deferred pending), verify it now that we have the local digest. */
    if (g_ble_ctx.peer_commitment_pending && transport_ble_local_commitment_ready()) {
        ESP_LOGI(TAG, "exchange_commitment: verifying deferred peer commitment now");
        (void)transport_ble_verify_pending_commitment();
    }

    g_ble_ctx.state = BLE_PAIRING;
    g_ble_ctx.pairing_start_ts = (uint32_t)(esp_timer_get_time() / 1000000LL);

    return CEEPEW_OK;
}

/* [FIX-4] Write verification result to GATTS 0xFFF2 characteristic.
 * Called by responder after verifying commitment to notify initiator of result. */
CeePewErr_t transport_ble_verify_commitment(const uint8_t *peer_digest, uint8_t len)
{
    CEEPEW_ASSERT(peer_digest != NULL, CEEPEW_ERR_NULL_PTR);
    if (g_ble_ctx.state != BLE_PAIRING && g_ble_ctx.state != BLE_CONNECTED) {
        return CEEPEW_ERR_PARAM;
    }
    CEEPEW_ASSERT(len == CEEPEW_COMMITMENT_BYTES || len == CEEPEW_COMMITMENT_LEGACY_BYTES, CEEPEW_ERR_PARAM);

    /* Defensive: record peer length */
    g_ble_ctx.peer_commitment_len = len;
    g_ble_ctx.peer_commitment_legacy = (len == CEEPEW_COMMITMENT_LEGACY_BYTES);

     /* Delegate verification (commitment + optional signature) to session layer.
      * session_verify_peer_commitment_with_sig performs constant-time commitment compare
      * and verifies an appended Ed25519 signature if present. */
     CeePewErr_t verr = session_verify_peer_commitment_with_sig(peer_digest, len);
     if (verr != CEEPEW_OK) {
         /* Defensive: record failure and log for diagnostics. Do NOT reveal details. */
         g_ble_ctx.verify_fail_count++;
         g_ble_ctx.verification_result = CEEPEW_VERIFY_MISMATCH;
         g_ble_ctx.peer_verification_result = CEEPEW_VERIFY_MISMATCH;
         g_ble_ctx.peer_verification_pending = false;
         g_ble_ctx.peer_commitment_read_issued = false;
         ESP_LOGW(TAG, "commitment verification failed — err=%d", (int)verr);
         return verr;
     }

     g_ble_ctx.commitment_verified = true;
     g_ble_ctx.verification_result = CEEPEW_VERIFY_OK;
     g_ble_ctx.peer_verification_result = CEEPEW_VERIFY_OK;
     g_ble_ctx.peer_verification_pending = false;
     g_ble_ctx.peer_commitment_read_issued = false;
     g_ble_ctx.verification_timeout_ms = 0U;
     g_ble_ctx.ready_for_chat = true;        /* Signal local readiness */
     g_ble_ctx.handoff_ready = true;
     g_ble_ctx.state = BLE_DONE;
     s_preverify_disconnect_deadline_ms = 0U;

     return CEEPEW_OK;
}

CeePewErr_t transport_ble_verify_pending_commitment(void)
{
    CEEPEW_ASSERT(s_ble_initialised, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(g_ble_ctx.peer_commitment_pending
                      ? (g_ble_ctx.pending_peer_commitment_len > 0U)
                      : true,
                  CEEPEW_ERR_PARAM);

    if (!g_ble_ctx.peer_commitment_pending) {
        return CEEPEW_OK;
    }

    /* FIX A1: If local commitment not yet loaded, defer verification instead of
     * failing immediately. session_fsm will call exchange_commitment() soon,
     * which will trigger retry via transport_ble_exchange_commitment(). */
    if (!transport_ble_local_commitment_ready()) {
        ESP_LOGW(TAG, "verify_pending: local commitment not ready — deferring");
        /* Keep peer_commitment_pending = true so it's retried after local
         * commitment is loaded */
        return CEEPEW_OK;
    }

    uint8_t pending_len = g_ble_ctx.pending_peer_commitment_len;
    g_ble_ctx.peer_commitment_pending = false;
    g_ble_ctx.pending_peer_commitment_len = 0U;

    CeePewErr_t err = transport_ble_verify_commitment(g_ble_ctx.pending_peer_commitment,
                                                      pending_len);
    ceepew_secure_zero(g_ble_ctx.pending_peer_commitment, (uint32_t)sizeof(g_ble_ctx.pending_peer_commitment));
    if (err == CEEPEW_OK) {
        ESP_LOGI(TAG, "Deferred commitment verification PASSED");
        /* FIX B: Set commitment_verified flag on responder side to match initiator behavior.
         * This unblocks transport_ble_both_ready_for_chat() so responder can proceed to
         * key derivation. Both devices now converge independently. */
        g_ble_ctx.commitment_verified = true;
        g_ble_ctx.peer_ready_for_chat = true;
        if (!g_ble_ctx.ready_for_chat) {
            transport_ble_set_ready_for_chat();
        }
        ESP_LOGI(TAG, "Responder: commitment_verified and peer_ready_for_chat set — ready for handoff");
        return CEEPEW_OK;
    }

    ESP_LOGW(TAG, "Deferred commitment verification failed: %d", (int)err);
    transport_ble_log_state_snapshot("Deferred commitment verification failed");
    g_ui_ctx.pairing_result_reason =
        (err == CEEPEW_ERR_AUTH_FAIL) ? UI_PAIRING_RESULT_COMMITMENT_FAIL
                                      : UI_PAIRING_RESULT_LINK_FAIL;
    (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
    g_ui_ctx.transition_ready = true;
    /* FIX B: Do NOT call disconnect here — UI state machine will call it
     * when entering PAIRING_FAILED state. Calling it here causes double-
     * disconnect which resets scan state and prevents re-advertisement. */

    return err;
}

bool transport_ble_handoff_ready(void)
{
    return g_ble_ctx.handoff_ready && g_ble_ctx.commitment_verified;
}

static CeePewErr_t transport_ble_send_ready_signal(void)
{
#ifdef CONFIG_BT_ENABLED
    if (!g_ble_ctx.gattc_connected || g_ble_ctx.gattc_char_handle == 0U) {
        return CEEPEW_ERR_BUSY;
    }

    uint8_t val = 0x01U;
    esp_err_t err = esp_ble_gattc_write_char(
        g_ble_ctx.gattc_if,
        g_ble_ctx.conn_id,
        g_ble_ctx.gattc_char_handle,
        1U,
        &val,
        ESP_GATT_WRITE_TYPE_RSP,
        ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send ready signal (gattc_write_char): %d", err);
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "Sent READY signal to peer");
    return CEEPEW_OK;
#else
    /* BLE disabled: simulate immediate success */
    (void)g_ble_ctx;
    return CEEPEW_OK;
#endif
}

void transport_ble_set_ready_for_chat(void)
{
    g_ble_ctx.ready_for_chat = true;
    g_ble_ctx.handoff_ready = g_ble_ctx.commitment_verified;
    ESP_LOGI(TAG, "Local ready_for_chat set to true");
}

bool transport_ble_peer_ready_for_chat(void)
{
    return g_ble_ctx.peer_ready_for_chat;
}

bool transport_ble_both_ready_for_chat(void)
{
    CEEPEW_ASSERT(g_ble_ctx.state <= BLE_DONE, false);
    CEEPEW_ASSERT(g_ble_ctx.local_commitment_len <= CEEPEW_COMMITMENT_BYTES, false);

    /*
     * Simplified: readiness converges independently on each side now that the
     * initiator advances on write ACK and the responder no longer needs a
     * reverse GATTC path.
     */
    bool ready_for_chat = g_ble_ctx.ready_for_chat;
    bool commitment_verified = g_ble_ctx.commitment_verified;
    bool handoff_ready = g_ble_ctx.handoff_ready;
    bool result = ready_for_chat && commitment_verified && handoff_ready;

    /* Debug logging: show which flag is blocking progression */
    static uint32_t last_log_ms = 0U;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    if (now_ms - last_log_ms >= 5000U) {  /* Log every 5 seconds */
        last_log_ms = now_ms;
        ESP_LOGI(TAG, "Both ready check: ready=%u commit_verified=%u handoff_ready=%u => %u",
                 ready_for_chat, commitment_verified, handoff_ready, result);
    }

    return result;
}

CeePewErr_t transport_ble_disconnect(void)
{
    if (g_ble_ctx.state == BLE_IDLE) {
        return CEEPEW_OK;
    }

    /* FIX: Save the pre-disconnect state before we clear GATT connections,
     * so we can check whether pairing was in progress. If we check after
     * clearing connections, state will already be BLE_IDLE and the check fails. */
    BleState_t pre_disconnect_state = g_ble_ctx.state;

#ifdef CONFIG_BT_ENABLED
    /* Attempt to disconnect the BLE link if we have a peer address recorded. */
    esp_err_t err = esp_ble_gap_disconnect(g_ble_ctx.peer_mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gap_disconnect returned %d", err);
        /* proceed to reset state regardless */
    }
#endif

    if (s_discovery_restart_pending) {
        g_ble_ctx.state = BLE_ADVERTISING_AND_SCANNING;
    } else {
        transport_ble_update_state_from_flags();
    }
    g_ble_ctx.handoff_ready = false;
    g_ble_ctx.gattc_connected = false;
    g_ble_ctx.gatts_connected = false;
    g_ble_ctx.connecting = false;
    g_ble_ctx.gatt_connected_since_ms = 0U;
    s_scan_requested = false;
    transport_ble_update_state_from_flags();

    /* FIX 2: After any disconnect that is NOT a clean handoff, re-arm
     * advertising so the device is discoverable again. Without this,
     * is_advertising stays false after the first disconnect and the device
     * goes silent. Only skip if a session is already active (Phase 3+) or if
     * we were in the middle of a Phase 2 pairing attempt (check pre-disconnect state). */
    if (!g_ble_ctx.commitment_verified && !session_is_active() &&
        pre_disconnect_state != BLE_PAIRING && pre_disconnect_state != BLE_CONNECTED) {
        s_adv_data_set  = false;
        s_scan_rsp_set  = false;
        s_adv_starting  = false;
        (void)transport_ble_start_advertising();
    }

    /* FIX C: Zero commitment digest on disconnect to prevent stale data
     * from being used in the next pairing attempt. This ensures
     * transport_ble_local_commitment_ready() doesn't return TRUE with
     * garbage/stale data from a previous failed pairing. */
    ceepew_secure_zero(g_ble_ctx.commitment_digest, sizeof(g_ble_ctx.commitment_digest));
    g_ble_ctx.local_commitment_len = 0U;
    ceepew_secure_zero(g_ble_ctx.pending_peer_commitment, sizeof(g_ble_ctx.pending_peer_commitment));
    g_ble_ctx.pending_peer_commitment_len = 0U;
    g_ble_ctx.peer_commitment_pending = false;
    g_ble_ctx.commitment_write_pending = false;
    g_ble_ctx.commitment_verified = false;

    ESP_LOGI(TAG, "BLE disconnected: state=%s adv=%u scan=%u (commitment data zeroed)",
             transport_ble_state_name(g_ble_ctx.state),
             g_ble_ctx.is_advertising ? 1U : 0U,
             g_ble_ctx.is_scanning ? 1U : 0U);

    return CEEPEW_OK;
}

CeePewErr_t transport_ble_deinit(void)
{
    CEEPEW_ASSERT(s_ble_initialised, CEEPEW_ERR_PARAM);

#ifdef CONFIG_BT_ENABLED
    esp_err_t err;
    err = esp_bluedroid_disable();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_bluedroid_disable returned %d", err);
    }
    err = esp_bluedroid_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_bluedroid_deinit returned %d", err);
    }
#endif

    memset(&g_ble_ctx, 0U, sizeof(BleContext_t));
    memset(g_ble_ctx.peer_name, 0U, sizeof(g_ble_ctx.peer_name));
    g_ble_ctx.peer_name_len = 0U;
    g_ble_ctx.peer_rssi = 0;
    s_scan_requested = false;
    s_adv_data_set = false;
    s_scan_rsp_set = false;
    s_adv_starting = false;
    s_scan_start_failed = false;
    s_scan_retry_after_ms = 0U;
    s_commitment_retry_after_ms = 0U;
    s_commitment_retry_count = 0U;
    s_ble_initialised = false;
    return CEEPEW_OK;
}

/* ════════════════════════════════════════════════════════════════════════
 * transport_ble_retry_scan_if_needed()
 *
 * Called periodically from task_session_run() (on the 1 s timeout tick).
 * If a previous scan-start failed (e.g., transient coexistence conflict),
 * this clears the request guard and retries.
 * ════════════════════════════════════════════════════════════════════════ */
CeePewErr_t transport_ble_retry_scan_if_needed(void)
{
    CEEPEW_ASSERT(s_ble_initialised, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(g_ble_ctx.state <= BLE_DONE, CEEPEW_ERR_PARAM);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    /* ── [FIX-2] Responder watchdog ──────────────────────────────────── */
    /* The responder has no GATTC path; it can only detect a stalled
     * initiator by timing out.  If we are in BLE_CONNECTED but have not
     * yet verified a commitment and the watchdog deadline has passed,
     * restart discovery so the user can try again.                       */
    if (s_responder_connected_at_ms != 0U &&
        !g_ble_ctx.commitment_verified &&
        !g_ble_ctx.handoff_ready &&
        !session_is_active()) {

        uint32_t elapsed = (now_ms > s_responder_connected_at_ms)
                           ? (now_ms - s_responder_connected_at_ms)
                           : 0U;
        if (elapsed > CEEPEW_RESPONDER_WATCHDOG_MS) {
            ESP_LOGW(TAG,
                     "Responder watchdog expired (%lu ms) — restarting discovery",
                     (unsigned long)elapsed);
            s_responder_connected_at_ms = 0U;  /* disarm watchdog */
            CeePewErr_t restart_err =
                transport_ble_restart_discovery_session();
            if (restart_err != CEEPEW_OK) {
                ESP_LOGE(TAG, "Responder watchdog restart failed: %d",
                         (int)restart_err);
            }
            return restart_err;
        }
    }

    /* ── Peer-lost timeout check ──────────────────────────────────────── */
    {
        /*
         * CRITICAL FIX:
         * Do NOT clear the discovered peer if:
         *   (a) a GATT connection is active (gattc or gatts side), OR
         *   (b) the commitment has been verified (pairing succeeded), OR
         *   (c) the session is now active (key derivation completed).
         *
         * The root cause of "only one device enters chat" was that after
         * key derivation the peer's last_seen_ms was ~19s stale (scan hits
         * stop during a GATT connection), so this function immediately
         * cleared the discovered peer, causing task_session to push the UI
         * back to DISCOVERY.
         */
        bool skip_clear = g_ble_ctx.gattc_connected
                       || g_ble_ctx.gatts_connected
                       || g_ble_ctx.commitment_verified
                       || g_ble_ctx.handoff_ready
                       || session_is_active();

        const uint32_t PEER_LOST_TIMEOUT_MS = CEEPEW_DISCOVERY_PEER_CLEAR_MS;

        if (!skip_clear
            && g_ble_ctx.discovered
            && g_ble_ctx.last_seen_ms != 0U
            && !transport_ble_peer_is_recent_at(now_ms, PEER_LOST_TIMEOUT_MS)) {

            ESP_LOGW(TAG, "Peer truly lost: last seen %lu ms ago — clearing discovered",
                     (unsigned long)(now_ms - g_ble_ctx.last_seen_ms));

            transport_ble_clear_discovery_peer_state();

            (void)ui_manager_transition_to(UI_STATE_DISCOVERY);

#ifdef CONFIG_BT_ENABLED
            (void)esp_ble_gap_stop_scanning();
            s_scan_requested    = false;
            s_scan_start_failed = false;
            (void)transport_ble_start_scan();
#endif
        }
    }

    /* ── Scan activity watchdog: detect a stalled scan where the BLE stack
     * reports scanning but no new peers have been observed for an extended
     * period. In that case, restart the discovery session to recover.        */
    if (g_ble_ctx.is_scanning ||
        g_ble_ctx.state == BLE_SCANNING ||
        g_ble_ctx.state == BLE_ADVERTISING_AND_SCANNING) {

        /* Safely sample scan_seen_count under mutex */
        ble_ctx_lock();
        uint32_t seen = g_ble_ctx.scan_seen_count;
        bool discovered = g_ble_ctx.discovered;
        ble_ctx_unlock();

        if (seen != s_last_scan_seen_value) {
            s_last_scan_seen_value = seen;
            s_last_scan_seen_change_ms = now_ms;
        } else {
            uint32_t idle = (now_ms > s_last_scan_seen_change_ms) ? (now_ms - s_last_scan_seen_change_ms) : 0U;
            if (idle > CEEPEW_SCAN_STUCK_TIMEOUT_MS && !discovered) {
                ESP_LOGW(TAG, "BLE scan appears stalled (%lu ms no new peers) — restarting discovery", (unsigned long)idle);
                CeePewErr_t restart_err = transport_ble_restart_discovery_session();
                if (restart_err != CEEPEW_OK) {
                    ESP_LOGE(TAG, "Discovery restart failed during scan-watchdog: %d", (int)restart_err);
                    return restart_err;
                }
                /* reset watchdog anchors */
                s_last_scan_seen_change_ms = now_ms;
                s_last_scan_seen_value = seen;
                return CEEPEW_OK;
            }
        }
    }

    /* ── Scan retry ────────────────────────────────────────────────────── */
    if (!s_scan_start_failed) { return CEEPEW_OK; }

    if (s_scan_retry_after_ms != 0U && now_ms < s_scan_retry_after_ms) {
        return CEEPEW_OK;
    }

    if (g_ble_ctx.is_scanning ||
        g_ble_ctx.state == BLE_SCANNING ||
        g_ble_ctx.state == BLE_ADVERTISING_AND_SCANNING) {
        ESP_LOGI(TAG, "Scan is already active; dropping retry request");
        s_scan_requested = false;
        s_scan_start_failed = false;
        s_scan_retry_after_ms = 0U;
        return CEEPEW_OK;
    }

    ESP_LOGW(TAG, "Retrying BLE scan start after previous failure");
    s_scan_requested    = false;
    s_scan_start_failed = false;
    s_scan_retry_after_ms = 0U;
    CeePewErr_t err = transport_ble_start_scan();
    if (err != CEEPEW_OK) {
        s_scan_start_failed = true;
        s_scan_retry_after_ms = now_ms + CEEPEW_RESTART_DEBOUNCE_MS;
    }
    return err;
}

CeePewErr_t transport_ble_retry_commitment_if_needed(void)
{
    CEEPEW_ASSERT(s_ble_initialised, CEEPEW_ERR_PARAM);

    /* If a peer verification is pending and we have the peer's verify characteristic handle,
       attempt to read the peer commitment (initiator path). This replaces the old
       client-write verification roundtrip. */
    if (g_ble_ctx.peer_verification_pending &&
        g_ble_ctx.gattc_verify_char_handle != 0U &&
        g_ble_ctx.gattc_connected &&
        !g_ble_ctx.peer_commitment_read_issued) {
        CeePewErr_t rr_err = transport_ble_request_peer_commitment_read();
        if (rr_err == CEEPEW_OK) {
            ESP_LOGI(TAG, "Requested peer commitment read on retry tick");
        } else if (rr_err != CEEPEW_ERR_BUSY) {
            ESP_LOGW(TAG, "Peer commitment read request failed (will retry): %d", (int)rr_err);
        }
    }

    if (!g_ble_ctx.commitment_write_pending) {
        return CEEPEW_OK;
    }

    if (!transport_ble_local_commitment_ready()) {
        return CEEPEW_ERR_PARAM;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (s_commitment_retry_after_ms != 0U && now_ms < s_commitment_retry_after_ms) {
        return CEEPEW_OK;
    }

    if (!g_ble_ctx.gattc_connected && !g_ble_ctx.connecting &&
        (g_ble_ctx.peer_mac[0] != 0U || g_ble_ctx.peer_mac[1] != 0U ||
         g_ble_ctx.peer_mac[2] != 0U || g_ble_ctx.peer_mac[3] != 0U ||
         g_ble_ctx.peer_mac[4] != 0U || g_ble_ctx.peer_mac[5] != 0U)) {
        CeePewErr_t connect_err = transport_ble_connect_to_peer(g_ble_ctx.peer_mac);
        if (connect_err != CEEPEW_OK && connect_err != CEEPEW_ERR_BUSY) {
            ESP_LOGW(TAG, "Responder GATTC connect attempt failed: %d", (int)connect_err);
        }
        return CEEPEW_OK;
    }

    CeePewErr_t err = transport_ble_send_commitment_write();
    if (err == CEEPEW_OK) {
        g_ble_ctx.commitment_write_pending = false;
        s_commitment_retry_after_ms = 0U;
        s_commitment_retry_count = 0U;
        return CEEPEW_OK;
    }

    if (err == CEEPEW_ERR_BUSY) {
        uint32_t retry_after_ms = now_ms + 400U;
        if (s_commitment_retry_after_ms < retry_after_ms) {
            s_commitment_retry_after_ms = retry_after_ms;
        }
        return CEEPEW_OK;
    }

    s_commitment_retry_count++;
    if (s_commitment_retry_count >= 15U) {
        g_ble_ctx.commitment_write_pending = false;
        s_commitment_retry_after_ms = 0U;
        transport_ble_log_state_snapshot("Commitment retry exhausted — failing pairing");
        g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_LINK_FAIL;
        (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
        g_ui_ctx.transition_ready = true;
        (void)transport_ble_disconnect();
        return CEEPEW_ERR_MAX_RETRIES;
    }

    s_commitment_retry_after_ms = now_ms + 600U;
    return CEEPEW_OK;
}

#ifdef CONFIG_BT_ENABLED
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param){
    ESP_LOGI(TAG, "gap_event_handler: event=%d", event);
    switch (event) {

    /* ── Advertising data accepted by stack → start advertising only ── */
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT: {
        s_adv_data_set = true;
        ESP_LOGI(TAG, "ADV_DATA_SET_COMPLETE (scan_rsp_ready=%u)", s_scan_rsp_set);
        if (s_adv_data_set && s_scan_rsp_set && !s_adv_starting) {
            /*
             * FIX: s_adv_starting is set to true BEFORE calling
             * esp_ble_gap_start_advertising(). This ensures that when the
             * second config-complete callback fires (SCAN_RSP in this case
             * would have fired first if order is reversed), it sees
             * s_adv_starting == true and skips the duplicate call.
             */
            s_adv_starting = true;
            esp_ble_adv_params_t adv_params = {
                .adv_int_min       = 0x20,
                .adv_int_max       = 0x40,
                .adv_type          = ADV_TYPE_IND,
                .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
                .channel_map       = ADV_CHNL_ALL,
                .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
            };
            esp_err_t e = esp_ble_gap_start_advertising(&adv_params);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "esp_ble_gap_start_advertising failed (from ADV_DATA_SET): %d (%s)",
                         e, esp_err_to_name(e));
                s_adv_starting = false;
            } else {
                ESP_LOGI(TAG, "Advertising start requested (from ADV_DATA_SET)");
                g_ble_ctx.adv_packet_count = 1U;
            }
        }
    } break;

    /* ── Scan response data accepted → no action needed ──────────────── */
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        s_scan_rsp_set = true;
        ESP_LOGI(TAG, "SCAN_RSP_DATA_SET_COMPLETE (adv_data_ready=%u)", s_adv_data_set);
        if (s_adv_data_set && s_scan_rsp_set && !s_adv_starting) {
            s_adv_starting = true;
            esp_ble_adv_params_t adv_params = {
                .adv_int_min       = 0x20,
                .adv_int_max       = 0x40,
                .adv_type          = ADV_TYPE_IND,
                .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
                .channel_map       = ADV_CHNL_ALL,
                .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
            };
            esp_err_t e = esp_ble_gap_start_advertising(&adv_params);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "esp_ble_gap_start_advertising failed (from SCAN_RSP_SET): %d (%s)",
                         e, esp_err_to_name(e));
                s_adv_starting = false;
            } else {
                ESP_LOGI(TAG, "Advertising start requested (from SCAN_RSP_SET)");
                g_ble_ctx.adv_packet_count = 1U;
            }
        }
        break;

    /* ── Scan parameters committed → NOW start scanning ──────────────── */
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
        if (!param) { break; }
        if (param->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            if ((param->scan_param_cmpl.status == 0x0CU) &&
                (g_ble_ctx.is_scanning || g_ble_ctx.state == BLE_SCANNING ||
                 g_ble_ctx.state == BLE_ADVERTISING_AND_SCANNING)) {
                ESP_LOGW(TAG, "Scan param set returned Cmd Disallowed while scan already active; ignoring");
                s_scan_requested = false;
                s_scan_start_failed = false;
                s_scan_retry_after_ms = 0U;
                break;
            }
            ESP_LOGE(TAG, "Scan param set FAILED: status=%d",
                     param->scan_param_cmpl.status);
            s_scan_requested    = false;
            s_scan_start_failed = true;
            s_scan_retry_after_ms = (uint32_t)(esp_timer_get_time() / 1000LL) +
                                    CEEPEW_RESTART_DEBOUNCE_MS;
            break;
        }
        {
            esp_err_t e = esp_ble_gap_start_scanning(0U); /* 0 = continuous */
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "esp_ble_gap_start_scanning FAILED: %d (%s)",
                         e, esp_err_to_name(e));
                s_scan_requested    = false;
                s_scan_start_failed = true; /* retry via transport_ble_retry_scan_if_needed */
                s_scan_retry_after_ms = (uint32_t)(esp_timer_get_time() / 1000LL) +
                                        CEEPEW_RESTART_DEBOUNCE_MS;
            } else {
                ESP_LOGI(TAG, "Scanning started: active, allow-all, continuous");
            }
        }
    } break;

    /* ── Scan start confirmed ─────────────────────────────────────────── */
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (!param) { break; }
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            g_ble_ctx.is_scanning   = true;
            s_scan_start_failed     = false;
            s_scan_requested        = false;
            s_scan_retry_after_ms   = 0U;
            ESP_LOGI(TAG, "Scan confirmed active");
        } else {
            if ((param->scan_start_cmpl.status == 0x0CU) &&
                (g_ble_ctx.is_scanning || g_ble_ctx.state == BLE_SCANNING ||
                 g_ble_ctx.state == BLE_ADVERTISING_AND_SCANNING)) {
                ESP_LOGW(TAG, "Scan start Cmd Disallowed while scan already active; ignoring");
                s_scan_requested = false;
                s_scan_start_failed = false;
                s_scan_retry_after_ms = 0U;
            }
            else {
                ESP_LOGE(TAG, "Scan start FAILED: status=%d",
                         param->scan_start_cmpl.status);
                s_scan_requested    = false;
                s_scan_start_failed = true;
                s_scan_retry_after_ms = (uint32_t)(esp_timer_get_time() / 1000LL) +
                                        CEEPEW_RESTART_DEBOUNCE_MS;
            }
        }
        transport_ble_update_state_from_flags();
        ESP_LOGI(TAG, "State after scan start: %s",
                 transport_ble_state_name(g_ble_ctx.state));
        break;

    /* ── Advertising start confirmed ──────────────────────────────────── */
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        s_adv_starting = false; /* clear guard regardless of outcome */
        if (!param) { break; }
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            g_ble_ctx.is_advertising = true;
            ESP_LOGI(TAG, "Advertising confirmed active");
        } else {
            ESP_LOGE(TAG, "Advertising start FAILED: status=%d",
                     param->adv_start_cmpl.status);
        }
        transport_ble_update_state_from_flags();
        ESP_LOGI(TAG, "State after adv start: %s",
                 transport_ble_state_name(g_ble_ctx.state));
        break;

    /* ── Scan result ──────────────────────────────────────────────────── */
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (!param) { break; }
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) { break; }
        if (g_ble_ctx.scan_seen_count < UINT32_MAX) { g_ble_ctx.scan_seen_count++; }

        /* Diagnostic: log scan event frequency */
        if (g_ble_ctx.scan_seen_count % 100U == 0U) {
            ESP_LOGI(TAG, "Scan activity: seen_count=%lu discovered=%u hits=%u state=%s",
                     (unsigned long)g_ble_ctx.scan_seen_count, g_ble_ctx.discovered ? 1U : 0U,
                     g_ble_ctx.scan_hit_count, transport_ble_state_name(g_ble_ctx.state));
        }

        char    found_name[17] = {0};
        uint8_t found_name_len = 0U;
        bool    service_uuid_match = false;
        uint8_t service_uuid_len = 0U;
        char    found_mac[18];
        transport_ble_format_mac(param->scan_rst.bda, found_mac);

        uint16_t total_adv_len = (uint16_t)(param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len);
        uint8_t *raw_adv = param->scan_rst.ble_adv;
        bool payload_has_ceepew_name = false;
        if (raw_adv != NULL && total_adv_len >= 6U) {
            payload_has_ceepew_name = transport_ble_payload_contains(raw_adv, total_adv_len,
                                                                     (const uint8_t *)"CEEPEW", 6U);
        }
        uint8_t *name_ptr = esp_ble_resolve_adv_data_by_type(
            raw_adv,
            total_adv_len,
            ESP_BLE_AD_TYPE_NAME_CMPL,
            &found_name_len);
        if (name_ptr == NULL) {
            name_ptr = esp_ble_resolve_adv_data_by_type(
                raw_adv,
                total_adv_len,
                ESP_BLE_AD_TYPE_NAME_SHORT,
                &found_name_len);
        }

        if (name_ptr != NULL && found_name_len > 0U) {
            if (found_name_len > 16U) {
                found_name_len = 16U;
            }
            memcpy(found_name, name_ptr, found_name_len);
            found_name[found_name_len] = '\0';
        } else if (payload_has_ceepew_name) {
            memcpy(found_name, "CEEPEW", 6U);
            found_name_len = 6U;
        }

        uint8_t *uuid_ptr = esp_ble_resolve_adv_data_by_type(
            raw_adv,
            total_adv_len,
            ESP_BLE_AD_TYPE_16SRV_CMPL,
            &service_uuid_len);
        if (uuid_ptr == NULL) {
            uuid_ptr = esp_ble_resolve_adv_data_by_type(
                raw_adv,
                total_adv_len,
                ESP_BLE_AD_TYPE_16SRV_PART,
                &service_uuid_len);
        }
        if (uuid_ptr != NULL && service_uuid_len >= sizeof(s_adv_service_uuid16) &&
            memcmp(uuid_ptr, s_adv_service_uuid16, sizeof(s_adv_service_uuid16)) == 0) {
            service_uuid_match = true;
        }

        /* Diagnostic: trace UUID parsing if peer might be CEE-PEW */
        if ((found_name_len >= 2U) || service_uuid_match || uuid_ptr != NULL || payload_has_ceepew_name) {
            ESP_LOGD(TAG, "UUID parse: uuid_ptr=%p uuid_len=%u expected_len=%lu match=%u",
                     uuid_ptr, service_uuid_len, (unsigned long)sizeof(s_adv_service_uuid16),
                     service_uuid_match ? 1U : 0U);
        }

        /* Accept a device whose name starts with "CEEPEW" or whose service UUID
         * advertises the CEEPEW service. */
        bool is_ceepew_peer = ((found_name_len >= 6U &&
                                memcmp(found_name, "CEEPEW", 6U) == 0) ||
                              service_uuid_match ||
                              payload_has_ceepew_name);

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        if (g_ble_ctx.discovered &&
            !g_ble_ctx.gattc_connected &&
            !g_ble_ctx.gatts_connected &&
            !g_ble_ctx.commitment_verified &&
            !g_ble_ctx.handoff_ready &&
            !session_is_active() &&
            !transport_ble_peer_is_recent_at(now_ms, CEEPEW_DISCOVERY_PEER_CLEAR_MS)) {
            ESP_LOGI(TAG, "Cached peer expired after %lu ms — clearing discovery cache",
                     (unsigned long)(now_ms - g_ble_ctx.last_seen_ms));
            transport_ble_clear_discovery_peer_state();
        }

        bool duplicate_same_peer = false;
        if (is_ceepew_peer &&
            transport_ble_scan_peer_is_duplicate(param->scan_rst.bda, now_ms)) {
            duplicate_same_peer =
                g_ble_ctx.discovered &&
                (memcmp(g_ble_ctx.peer_mac, param->scan_rst.bda, 6U) == 0);
            if (!duplicate_same_peer) {
                break;
            }
        }

        ESP_LOGI(TAG, "Scan signal: evt=%u peer=%s rssi=%d adv_len=%u name=%s svc=%s match=%s",
                 (unsigned)param->scan_rst.search_evt,
                 found_mac,
                 (int)param->scan_rst.rssi,
                 (unsigned)param->scan_rst.adv_data_len,
                 (found_name_len > 0U) ? found_name : "<none>",
                 service_uuid_match ? "0xFFF0" : "none",
                 is_ceepew_peer ? "ceepew" : "other");

        if (!is_ceepew_peer) { break; }

        int16_t new_rssi_x8 = (int16_t)((int16_t)param->scan_rst.rssi * 8);

        if (!g_ble_ctx.discovered) {
            ESP_LOGI(TAG, "CEEPEW peer first seen: %02X:%02X:%02X:%02X:%02X:%02X "
                     "RSSI=%d name=%s",
                     param->scan_rst.bda[0], param->scan_rst.bda[1],
                     param->scan_rst.bda[2], param->scan_rst.bda[3],
                     param->scan_rst.bda[4], param->scan_rst.bda[5],
                     param->scan_rst.rssi, found_name);

            memcpy(g_ble_ctx.peer_mac,   param->scan_rst.bda, 6U);
            g_ble_ctx.peer_addr_type     = (esp_ble_addr_type_t)param->scan_rst.ble_addr_type;
            memcpy(g_ble_ctx.peer_name,  found_name, found_name_len);
            g_ble_ctx.peer_name_len       = found_name_len;
            g_ble_ctx.peer_rssi           = (int8_t)param->scan_rst.rssi;
            g_ble_ctx.peer_rssi_smooth_x8 = new_rssi_x8;
            g_ble_ctx.last_seen_ms        = (uint32_t)(esp_timer_get_time() / 1000LL);
            g_ble_ctx.scan_hit_count      = 1U;
            g_ble_ctx.discovered          = true;
            g_ble_ctx.discovery_start_ts  =
                (uint32_t)(esp_timer_get_time() / 1000000LL);

            memcpy(g_ble_ctx.peer_record.peer_mac, g_ble_ctx.peer_mac, 6U);
            g_ble_ctx.peer_record.rssi = g_ble_ctx.peer_rssi;
            g_ble_ctx.peer_record.seen_at = g_ble_ctx.discovery_start_ts;
            g_ble_ctx.peer_record.name_len = found_name_len;
            memcpy(g_ble_ctx.peer_record.name, found_name, found_name_len);
            g_ble_ctx.peer_record.name[g_ble_ctx.peer_record.name_len] = '\0';
            transport_ble_log_peer_snapshot("Peer accepted:", &g_ble_ctx.peer_record);

            (void)ui_manager_transition_to(UI_STATE_DISCOVERY);

        } else if (memcmp(g_ble_ctx.peer_mac, param->scan_rst.bda, 6U) == 0) {
            /* Same peer — update EMA */
            g_ble_ctx.peer_rssi = (int8_t)param->scan_rst.rssi;
            g_ble_ctx.peer_rssi_smooth_x8 =
                (int16_t)((6 * g_ble_ctx.peer_rssi_smooth_x8 + 2 * new_rssi_x8) / 8);
            g_ble_ctx.last_seen_ms =
                (uint32_t)(esp_timer_get_time() / 1000LL);
            if (g_ble_ctx.scan_hit_count < 255U) { g_ble_ctx.scan_hit_count++; }

            g_ble_ctx.peer_record.rssi = g_ble_ctx.peer_rssi;
            g_ble_ctx.peer_record.seen_at =
                (uint32_t)(esp_timer_get_time() / 1000000LL);
            transport_ble_log_peer_snapshot("Peer updated:", &g_ble_ctx.peer_record);
        }
    } break;

    default:
        break;
    }
}
static void gattc_event_handler(esp_gattc_cb_event_t event,
                                esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    if (!param) { return; }

    switch (event) {
        case ESP_GATTC_REG_EVT:
            if (param->reg.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "GATTC register failed: %d", param->reg.status);
                break;
            }
            g_ble_ctx.gattc_if = gattc_if;
            g_ble_ctx.gattc_registered = true;
            ESP_LOGI(TAG, "GATTC registered, if=%d", gattc_if);
            /* Start scanning now that GATTC is registered and stack is ready */
            (void)transport_ble_start_scan();
            break;

        case ESP_GATTC_OPEN_EVT:
            if (param->open.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "GATTC open failed: %d", param->open.status);
                g_ble_ctx.connecting = false;
            }
            break;

        case ESP_GATTC_CONNECT_EVT:
            g_ble_ctx.conn_id = param->connect.conn_id;
            g_ble_ctx.gattc_connected = true;
            g_ble_ctx.gatt_connected_since_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            g_ble_ctx.connecting = false;
            g_ble_ctx.state = BLE_CONNECTED;
            g_ble_ctx.service_start_handle = 0U;
            g_ble_ctx.service_end_handle = 0U;
            g_ble_ctx.reconnect_attempts = 0U;
            s_write_attempt = 0U;
            s_commitment_retry_count = 0U;
            s_commitment_retry_after_ms = 0U;
            s_preverify_disconnect_deadline_ms = 0U;
            s_gattc_mtu_ready = false;
            ESP_LOGI(TAG, "GATTC connected: conn_id=%u", (unsigned)g_ble_ctx.conn_id);
            {
                esp_err_t mtu_err = esp_ble_gattc_send_mtu_req(gattc_if, g_ble_ctx.conn_id);
                /* If verification pending, request peer commitment read (initiator path). */
                if (g_ble_ctx.peer_verification_pending && g_ble_ctx.gattc_verify_char_handle != 0U) {
                    CeePewErr_t rr_err = transport_ble_request_peer_commitment_read();
                    if (rr_err == CEEPEW_OK) {
                        ESP_LOGI(TAG, "Requested peer commitment read on connect");
                    } else if (rr_err != CEEPEW_ERR_BUSY) {
                        ESP_LOGW(TAG, "Peer commitment read request on connect failed: %d", (int)rr_err);
                    }
                }
                if (mtu_err == ESP_OK) {
                    ESP_LOGI(TAG, "GATTC MTU request sent");
                } else {
                    ESP_LOGW(TAG, "GATTC MTU request failed: %d", mtu_err);
                }
            }
            (void)esp_ble_gattc_search_service(gattc_if, g_ble_ctx.conn_id,
                                               &s_service_uuid_filter);
            break;

        case ESP_GATTC_CFG_MTU_EVT:
            if (param->cfg_mtu.status == ESP_GATT_OK) {
                s_gattc_mtu_ready = true;
                ESP_LOGI(TAG, "GATTC MTU configured: %u", (unsigned)param->cfg_mtu.mtu);
                if (g_ble_ctx.commitment_write_pending && g_ble_ctx.gattc_char_handle != 0U) {
                    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                    /* Bug 2 Fix: Overwrite any stale long-delay schedule set before MTU was confirmed */
                    s_commitment_retry_after_ms = now_ms + 50U;
                    ESP_LOGI(TAG, "MTU confirmed — scheduling pending write in 50ms");
                }
            } else {
                ESP_LOGW(TAG, "GATTC MTU config failed: %d", param->cfg_mtu.status);
            }
            break;

        case ESP_GATTC_SEARCH_RES_EVT:
            if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
                param->search_res.srvc_id.uuid.uuid.uuid16 == BLE_SERVICE_UUID) {
                g_ble_ctx.service_start_handle = param->search_res.start_handle;
                g_ble_ctx.service_end_handle = param->search_res.end_handle;
                ESP_LOGI(TAG, "GATTC service found: %u-%u",
                         g_ble_ctx.service_start_handle,
                         g_ble_ctx.service_end_handle);
            }
            break;

        case ESP_GATTC_SEARCH_CMPL_EVT: {
            if (param->search_cmpl.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "GATTC service search failed: %d", param->search_cmpl.status);
                break;
            }
            if (g_ble_ctx.service_start_handle == 0U ||
                g_ble_ctx.service_end_handle == 0U) {
                ESP_LOGW(TAG, "GATTC service range unavailable");
                break;
            }
            esp_gattc_char_elem_t char_elem;
            uint16_t count = 1U;
            esp_err_t err = esp_ble_gattc_get_char_by_uuid(
                gattc_if,
                g_ble_ctx.conn_id,
                g_ble_ctx.service_start_handle,
                g_ble_ctx.service_end_handle,
                s_char_uuid,
                &char_elem,
                &count);
            if (err == ESP_OK && count > 0U) {
                g_ble_ctx.gattc_char_handle = char_elem.char_handle;
                ESP_LOGI(TAG, "GATTC characteristic ready: handle=%u",
                         (unsigned)g_ble_ctx.gattc_char_handle);
                if (g_ble_ctx.commitment_write_pending) {
                    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                    if (!s_gattc_mtu_ready) {
                        uint32_t delay = 1500U;  /* Bug 2 Fix: Wait longer for MTU negotiation */
                        s_commitment_retry_after_ms = now_ms + delay;
                        ESP_LOGI(TAG, "GATTC char found — scheduling write in %u ms (mtu_ready=%u)",
                                 (unsigned)delay, (unsigned)s_gattc_mtu_ready);
                    } else {
                        s_commitment_retry_after_ms = now_ms + 50U;
                        ESP_LOGI(TAG, "GATTC char found and MTU ready — scheduling pending commitment write");
                    }
                }
            } else {
                ESP_LOGW(TAG, "GATTC characteristic not found");
            }

            /* Also attempt to locate verification status characteristic (0xFFF2) */
            {
                esp_gattc_char_elem_t verify_elem;
                uint16_t count2 = 1U;
                esp_err_t err2 = esp_ble_gattc_get_char_by_uuid(
                    gattc_if,
                    g_ble_ctx.conn_id,
                    g_ble_ctx.service_start_handle,
                    g_ble_ctx.service_end_handle,
                    s_verify_char_uuid,
                    &verify_elem,
                    &count2);
                if (err2 == ESP_OK && count2 > 0U) {
                    g_ble_ctx.gattc_verify_char_handle = verify_elem.char_handle;
                    ESP_LOGI(TAG, "GATTC verify characteristic ready: handle=%u",
                             (unsigned)g_ble_ctx.gattc_verify_char_handle);
                    /* If we are waiting for a peer verification, request the peer's commitment value */
                    if (g_ble_ctx.peer_verification_pending) {
                        CeePewErr_t rr_err = transport_ble_request_peer_commitment_read();
                        if (rr_err == CEEPEW_OK) {
                            ESP_LOGI(TAG, "Requested peer commitment read after service discovery");
                        } else if (rr_err != CEEPEW_ERR_BUSY) {
                            ESP_LOGW(TAG, "Peer commitment read request failed: %d", (int)rr_err);
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "GATTC verify characteristic not found");
                }
            }
        } break;

        case ESP_GATTC_WRITE_CHAR_EVT: {
            ESP_LOGI(TAG, "GATTC_WRITE_CHAR_EVT: status=%d handle=%u",
                     param->write.status, (unsigned)param->write.handle);

            if (param->write.status == ESP_GATT_OK) {
                /* Write ACK — success path */
                s_write_attempt = 0U;

                ESP_LOGI(TAG, "GATTC write complete — commitment sent to peer");

                uint8_t local_zero = 0U;
                uint8_t cmp_len = (g_ble_ctx.local_commitment_len == 0U)
                                  ? (uint8_t)CEEPEW_COMMITMENT_BYTES
                                  : g_ble_ctx.local_commitment_len;
                for (uint8_t i = 0U; i < cmp_len; i++) {
                    local_zero |= g_ble_ctx.commitment_digest[i];
                }

                if (local_zero == 0U) {
                    ESP_LOGW(TAG, "GATTC write ACK with empty commitment — aborting");
                    g_ble_ctx.verify_fail_count++;
                    g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_COMMITMENT_FAIL;
                    (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
                    g_ui_ctx.transition_ready = true;
                    (void)transport_ble_disconnect();
                } else {
                    /*
                     * INITIATOR PATH: Write ACK proves the responder received our
                     * commitment. Both devices derive from the same session code,
                     * so we can advance the handshake without a reverse GATTC path.
                     */
                    g_ble_ctx.commitment_write_pending = false;
                    s_commitment_retry_after_ms = 0U;
                    s_commitment_retry_count = 0U;
                    g_ble_ctx.commitment_verified = true;
                    g_ble_ctx.handoff_ready = true;
                    g_ble_ctx.peer_ready_for_chat = true;
                    g_ble_ctx.ready_for_chat = true;
                    g_ble_ctx.state = BLE_DONE;

                    if (g_ble_ctx.peer_commitment_pending) {
                        (void)transport_ble_verify_pending_commitment();
                    }

                    ESP_LOGI(TAG, "Initiator: handoff_ready set on write ACK — proceeding to key derivation");
                }
            } else {
                /* Non-OK status — decide retryable vs fatal */
                if (param->write.status == ESP_GATT_ERR_UNLIKELY) {
                    /* Transient stack timing/MTU error — retry with backoff */
                    s_write_attempt++;
                    ESP_LOGW(TAG, "GATTC write transient error (133) — attempt %u/%u",
                             (unsigned)s_write_attempt, (unsigned)CEEPEW_WRITE_MAX_ATTEMPTS);
                    if (s_write_attempt < CEEPEW_WRITE_MAX_ATTEMPTS) {
                        g_ble_ctx.commitment_write_pending = true;
                        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                        /* linear backoff scaled by attempt number */
                        s_commitment_retry_after_ms = now_ms + (CEEPEW_WRITE_RETRY_DELAY_MS * (uint32_t)s_write_attempt);
                        ESP_LOGI(TAG, "Scheduled retry in %u ms", (unsigned)(s_commitment_retry_after_ms - now_ms));
                        /* Keep connection alive; do not disconnect here */
                    } else {
                        /* Exhausted retries — treat as link failure */
                        s_write_attempt = 0U;
                        ESP_LOGE(TAG, "GATTC write failed after %u attempts — pairing failed",
                                 (unsigned)CEEPEW_WRITE_MAX_ATTEMPTS);
                        g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_LINK_FAIL;
                        transport_ble_log_state_snapshot("GATTC write exhausted — failing pairing");
                        (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
                        g_ui_ctx.transition_ready = true;
                        (void)transport_ble_disconnect();
                    }
                } else {
                    /* Non-retryable GATT error — fail fast */
                    s_write_attempt = 0U;
                    ESP_LOGE(TAG, "GATTC write fatal error: status=%d — failing pairing", param->write.status);
                    transport_ble_log_state_snapshot("GATTC write fatal error — failing pairing");
                    g_ui_ctx.pairing_result_reason = transport_ble_reason_from_gatt_status(param->write.status);
                    (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
                    g_ui_ctx.transition_ready = true;
                    (void)transport_ble_disconnect();
                }
            }
        } break;

        case ESP_GATTC_READ_CHAR_EVT: {
            if (param->read.status == ESP_GATT_OK) {
                uint16_t len = param->read.value_len;
                if (len == CEEPEW_COMMITMENT_BYTES || len == CEEPEW_COMMITMENT_LEGACY_BYTES) {
                    ble_ctx_lock();
                    g_ble_ctx.pending_peer_commitment_len = (uint8_t)len;
                    memcpy(g_ble_ctx.pending_peer_commitment, param->read.value, len);
                    g_ble_ctx.peer_commitment_pending = true;
                    g_ble_ctx.peer_commitment_legacy = (len == CEEPEW_COMMITMENT_LEGACY_BYTES);
                    g_ble_ctx.peer_commitment_read_issued = false;
                    ble_ctx_unlock();
                    ESP_LOGI(TAG, "GATTC read complete — peer commitment (%u bytes) received", (unsigned)len);
                    (void)transport_ble_verify_pending_commitment();
                } else {
                    ESP_LOGW(TAG, "GATTC read returned unexpected length: %u", (unsigned)len);
                }
            } else {
                ESP_LOGW(TAG, "GATTC read failed: status=%d", param->read.status);
                ble_ctx_lock();
                g_ble_ctx.peer_commitment_read_issued = false;
                ble_ctx_unlock();
            }
        } break;

        case ESP_GATTC_DISCONNECT_EVT:
            g_ble_ctx.gattc_connected = false;
            g_ble_ctx.connecting = false;
            g_ble_ctx.gattc_char_handle = 0U;
            ESP_LOGI(TAG, "GATTC disconnected");

            /* Accumulate connected duration for current peer */
            {
                uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                ble_ctx_lock();
                if (g_ble_ctx.gatt_connected_since_ms != 0U) {
                    uint32_t delta = (now_ms > g_ble_ctx.gatt_connected_since_ms)
                                      ? (now_ms - g_ble_ctx.gatt_connected_since_ms) : 0U;
                    if (delta > 0U) {
                        if (g_ble_ctx.accumulated_conn_ms <= UINT32_MAX - delta) {
                            g_ble_ctx.accumulated_conn_ms += delta;
                        } else {
                            g_ble_ctx.accumulated_conn_ms = UINT32_MAX;
                        }
                    }
                    g_ble_ctx.gatt_connected_since_ms = 0U;
                }
                ble_ctx_unlock();
            }

            bool pairing_failure_visible_gattc =
                (g_ui_ctx.current_state == UI_STATE_PAIRING_FAILED ||
                 g_ui_ctx.next_state == UI_STATE_PAIRING_FAILED);
            if (pairing_failure_visible_gattc) {
                g_ble_ctx.reconnect_attempts = 0U;
                g_ble_ctx.state = BLE_IDLE;
                break;
            }

            /* If disconnect occurred before successful commitment verification,
             * attempt a limited number of reconnects before clearing the peer. */
            if (!g_ble_ctx.commitment_verified && !g_ble_ctx.handoff_ready) {
                if (g_ble_ctx.reconnect_attempts < CEEPEW_MAX_RECONNECT_ATTEMPTS &&
                    (g_ble_ctx.peer_mac[0] != 0U || g_ble_ctx.peer_mac[1] != 0U)) {
                    g_ble_ctx.reconnect_attempts++;
                    ESP_LOGW(TAG, "Disconnect before verification — attempting reconnect %u/%u",
                             (unsigned)g_ble_ctx.reconnect_attempts, (unsigned)CEEPEW_MAX_RECONNECT_ATTEMPTS);
                    transport_ble_update_state_from_flags();
                    (void)transport_ble_connect_to_peer(g_ble_ctx.peer_mac);
                } else {
                    ESP_LOGW(TAG, "Reconnect attempts exhausted — reporting pairing failure");
                    g_ble_ctx.reconnect_attempts = 0U;
                    transport_ble_log_state_snapshot("Reconnect attempts exhausted — failing pairing");
                    g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_LINK_FAIL;
                    (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
                    g_ui_ctx.transition_ready = true;
                }
            } else {
                g_ble_ctx.state = BLE_IDLE;
            }
            break;

        default:
            break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    if (!param) { return; }

    switch (event) {
        case ESP_GATTS_REG_EVT:
            if (param->reg.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "GATTS register failed: %d", param->reg.status);
                break;
            }
            g_ble_ctx.gatts_if = gatts_if;
            g_ble_ctx.gatts_registered = true;
            ESP_LOGI(TAG, "GATTS registered, if=%d", gatts_if);
            (void)esp_ble_gatts_create_service(gatts_if, &s_service_id, 4U);
            break;

        case ESP_GATTS_CREATE_EVT:
            g_ble_ctx.service_start_handle = param->create.service_handle;
            g_ble_ctx.service_end_handle = param->create.service_handle;
            ESP_LOGI(TAG, "GATTS service created: handle=%u",
                     (unsigned)param->create.service_handle);
            (void)esp_ble_gatts_start_service(param->create.service_handle);
            (void)esp_ble_gatts_add_char(
                param->create.service_handle,
                &s_char_uuid,
                ESP_GATT_PERM_WRITE,
                ESP_GATT_CHAR_PROP_BIT_WRITE,
                NULL,
                NULL);
            /* [FIX-4] Also add verification status characteristic (0xFFF2) */
            (void)esp_ble_gatts_add_char(
                param->create.service_handle,
                &s_verify_char_uuid,
                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                NULL,
                NULL);
            break;

        case ESP_GATTS_ADD_CHAR_EVT:
            /* Track which characteristic was added */
            if (g_ble_ctx.gatts_char_handle == 0U) {
                /* First characteristic is commitment (0xFFF1) */
                g_ble_ctx.gatts_char_handle = param->add_char.attr_handle;
                ESP_LOGI(TAG, "GATTS commitment characteristic added: handle=%u",
                         (unsigned)g_ble_ctx.gatts_char_handle);
            } else if (g_ble_ctx.gatts_verify_char_handle == 0U) {
                /* Second characteristic is verification status (0xFFF2) */
                g_ble_ctx.gatts_verify_char_handle = param->add_char.attr_handle;
                ESP_LOGI(TAG, "GATTS verification characteristic added: handle=%u",
                         (unsigned)g_ble_ctx.gatts_verify_char_handle);
                /* GATTS fully ready — safe to start advertising now */
                (void)transport_ble_start_advertising();
            }
            break;

        case ESP_GATTS_CONNECT_EVT:
            g_ble_ctx.gatts_connected   = true;
            g_ble_ctx.conn_id           = param->connect.conn_id;
            g_ble_ctx.gatt_connected_since_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            g_ble_ctx.state             = BLE_CONNECTED;
            g_ble_ctx.is_advertising    = false;
            g_ble_ctx.reconnect_attempts = 0U;
            s_preverify_disconnect_deadline_ms = 0U;
            /* [FIX-3] Reset write state and arm the responder watchdog */
            s_write_attempt = 0U;
            s_commitment_retry_count = 0U;
            s_commitment_retry_after_ms = 0U;
            s_responder_connected_at_ms =
                (uint32_t)(esp_timer_get_time() / 1000ULL);
            ESP_LOGI(TAG,
                     "GATTS connected: conn_id=%u state=BLE_CONNECTED watchdog_armed",
                     (unsigned)g_ble_ctx.conn_id);
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            g_ble_ctx.gatts_connected = false;
            ESP_LOGI(TAG, "GATTS disconnected");

           /* Accumulate connected duration for current peer */
           {
               uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
               ble_ctx_lock();
               if (g_ble_ctx.gatt_connected_since_ms != 0U) {
                   uint32_t delta = (now_ms > g_ble_ctx.gatt_connected_since_ms)
                                     ? (now_ms - g_ble_ctx.gatt_connected_since_ms) : 0U;
                   if (delta > 0U) {
                       if (g_ble_ctx.accumulated_conn_ms <= UINT32_MAX - delta) {
                           g_ble_ctx.accumulated_conn_ms += delta;
                       } else {
                           g_ble_ctx.accumulated_conn_ms = UINT32_MAX;
                       }
                   }
                   g_ble_ctx.gatt_connected_since_ms = 0U;
               }
               ble_ctx_unlock();
           }

           bool pairing_failure_visible_gatts =
               (g_ui_ctx.current_state == UI_STATE_PAIRING_FAILED ||
                g_ui_ctx.next_state == UI_STATE_PAIRING_FAILED);
           if (pairing_failure_visible_gatts) {
               g_ble_ctx.state = BLE_IDLE;
               break;
           }

           /* If responder disconnected before verification, allow a 3-second grace window
           * for transient disconnect recovery before hard-failing the pairing session. */
           if (!g_ble_ctx.commitment_verified && !g_ble_ctx.handoff_ready) {
               uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                
               /* If grace deadline not yet armed, arm it now */
               if (s_preverify_disconnect_deadline_ms == 0U) {
                   s_preverify_disconnect_deadline_ms = now_ms + CEEPEW_PREVERIFY_RETRY_WINDOW_MS;
                   ESP_LOGW(TAG, "Responder disconnect during pairing — grace window armed (%u ms)",
                            (unsigned)CEEPEW_PREVERIFY_RETRY_WINDOW_MS);
                   return; /* Allow reconnect attempts within the grace window */
               }

               /* Grace window has elapsed; classify as final failure */
               if (now_ms >= s_preverify_disconnect_deadline_ms) {
                   ESP_LOGI(TAG, "Responder grace window expired — reporting pairing failure");
                   transport_ble_clear_discovery_peer_state();
                   transport_ble_log_state_snapshot("Responder grace window expired — failing pairing");
                   g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_LINK_FAIL;
                   (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
                   g_ui_ctx.transition_ready = true;
                   s_preverify_disconnect_deadline_ms = 0U;
               } else {
                   /* Still within grace window — allow reconnect */
                   uint32_t remaining_ms = s_preverify_disconnect_deadline_ms - now_ms;
                   ESP_LOGW(TAG, "Responder disconnect within grace window (%u ms remaining) — allowing reconnect",
                            (unsigned)remaining_ms);
               }
           }
           break;

        case ESP_GATTS_EXEC_WRITE_EVT:
            /* Finalize a series of prepare writes (execute or cancel).
             * If exec_write.exec_write == true, the stack indicates the client
             * requested execution of the previously received prepare writes.
             * Otherwise the prepare sequence was cancelled. */
            if (param->exec_write.exec_write_flag) {
                if (g_ble_ctx.pending_peer_commitment_len == CEEPEW_COMMITMENT_BYTES ||
                    g_ble_ctx.pending_peer_commitment_len == CEEPEW_COMMITMENT_LEGACY_BYTES) {
                    g_ble_ctx.peer_commitment_pending = true;
                    s_responder_connected_at_ms = 0U;  /* disarm watchdog */
                    ESP_LOGI(TAG, "GATTS: exec write — buffered peer commitment (%u bytes) for deferred verification",
                             (unsigned)g_ble_ctx.pending_peer_commitment_len);
                } else {
                    ESP_LOGW(TAG, "GATTS: exec write — unexpected pending length=%u; discarding",
                             (unsigned)g_ble_ctx.pending_peer_commitment_len);
                    g_ble_ctx.pending_peer_commitment_len = 0U;
                    ceepew_secure_zero(g_ble_ctx.pending_peer_commitment, (uint32_t)sizeof(g_ble_ctx.pending_peer_commitment));
                }
            } else {
                /* Cancelled prepare write sequence — clear buffer */
                g_ble_ctx.pending_peer_commitment_len = 0U;
                ceepew_secure_zero(g_ble_ctx.pending_peer_commitment, (uint32_t)sizeof(g_ble_ctx.pending_peer_commitment));
                ESP_LOGI(TAG, "GATTS: exec write cancelled — cleared prepared buffer");
            }
            break;

        case ESP_GATTS_READ_EVT: {
            ESP_LOGI(TAG, "GATTS_READ_EVT: handle=%u conn_id=%u", (unsigned)param->read.handle, (unsigned)param->read.conn_id);
            if (param->read.handle == g_ble_ctx.gatts_verify_char_handle) {
                esp_gatt_rsp_t rsp = {0};
                rsp.attr_value.handle = param->read.handle;
                uint16_t len = (g_ble_ctx.local_commitment_len == 0U) ? CEEPEW_COMMITMENT_BYTES : g_ble_ctx.local_commitment_len;
                rsp.attr_value.len = len;
                memcpy(rsp.attr_value.value, g_ble_ctx.commitment_digest, len);
                (void)esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, &rsp);
                ESP_LOGI(TAG, "GATTS: answered read for verify char (len=%u)", (unsigned)len);
            } else {
                /* Let stack handle other reads */
                esp_gatt_rsp_t rsp = {0};
                (void)esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, &rsp);
            }
        } break;

        case ESP_GATTS_WRITE_EVT:
            if (param->write.need_rsp) {
                /* Initialize response struct — passing NULL causes "Invalid parameters: p_msg should not be NULL" */
                esp_gatt_rsp_t rsp = {0};
                rsp.attr_value.handle = param->write.handle;
                rsp.attr_value.len = 0U;
                (void)esp_ble_gatts_send_response(
                    gatts_if,
                    param->write.conn_id,
                    param->write.trans_id,
                    ESP_GATT_OK,
                    &rsp);
            }

            /* Handle prepare writes by accumulating into the pending buffer.
             * The prepare-write flow arrives as multiple WRITE_EVT with is_prep=true
             * followed by an EXEC_WRITE_EVT. The server must accumulate fragments
             * at the provided offset and wait for EXEC_WRITE to finalize. */
            if (param->write.is_prep && param->write.handle == g_ble_ctx.gatts_char_handle) {
                uint16_t offset = param->write.offset;
                uint16_t len = param->write.len;
                /* Bounds check — do not overflow our fixed-size buffer */
                if ((uint32_t)offset + (uint32_t)len > sizeof(g_ble_ctx.pending_peer_commitment)) {
                    ESP_LOGW(TAG, "GATTS: prepare write overflow offset=%u len=%u — rejecting", (unsigned)offset, (unsigned)len);
                    /* Clear any existing prepared data to avoid partial/poisoned state */
                    g_ble_ctx.pending_peer_commitment_len = 0U;
                    ceepew_secure_zero(g_ble_ctx.pending_peer_commitment, (uint32_t)sizeof(g_ble_ctx.pending_peer_commitment));
                } else {
                    memcpy(&g_ble_ctx.pending_peer_commitment[offset], param->write.value, len);
                    if (g_ble_ctx.pending_peer_commitment_len < (uint8_t)(offset + len)) {
                        g_ble_ctx.pending_peer_commitment_len = (uint8_t)(offset + len);
                    }
                    ESP_LOGI(TAG, "GATTS: received prep write offset=%u len=%u total=%u", (unsigned)offset, (unsigned)len, (unsigned)g_ble_ctx.pending_peer_commitment_len);
                }
                /* Prepared fragment accepted; wait for EXEC_WRITE to finalize */
                break;
            }

            /* Non-prepare writes (normal short or long writes already assembled by stack) */
            if (!param->write.is_prep && param->write.handle == g_ble_ctx.gatts_char_handle) {
                if (param->write.len == CEEPEW_COMMITMENT_BYTES || param->write.len == CEEPEW_COMMITMENT_LEGACY_BYTES) {
                    memcpy(g_ble_ctx.pending_peer_commitment, param->write.value, param->write.len);
                    g_ble_ctx.pending_peer_commitment_len = (uint8_t)param->write.len;
                    g_ble_ctx.peer_commitment_pending = true;
                    s_responder_connected_at_ms = 0U;  /* [FIX-3] disarm watchdog */
                    ESP_LOGI(TAG, "GATTS: buffered peer commitment (%u bytes) for deferred verification",
                             (unsigned)param->write.len);
                } else if (param->write.len == 1U && param->write.value[0] == 0x01U) {
                    /* Peer signalled READY for chat; record timestamp and flag */
                    g_ble_ctx.peer_ready_for_chat = true;
                    g_ble_ctx.peer_ready_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                    s_responder_connected_at_ms = 0U;  /* [FIX-3] disarm watchdog */
                    ESP_LOGI(TAG, "GATTS: peer signalled READY (timestamp=%lu)", (unsigned long)g_ble_ctx.peer_ready_timestamp_ms);
                    if (!g_ble_ctx.commitment_verified &&
                        g_ble_ctx.ready_for_chat &&
                        transport_ble_local_commitment_ready()) {
                        g_ble_ctx.commitment_verified = true;
                        g_ble_ctx.handoff_ready = true;
                        g_ble_ctx.state = BLE_DONE;
                        ESP_LOGI(TAG, "READY handshake complete: promoting initiator to verified handoff");
                    }
                    /* If both ready, UI may proceed; UI thread polls transport_ble_both_ready_for_chat() */
                } else if (!param->write.is_prep && param->write.handle == g_ble_ctx.gatts_verify_char_handle) {
                    ESP_LOGI(TAG, "GATTS: write to verify characteristic ignored (readable commitment is served via read)");
                }
            }
            break;

        default:
            break;
    }
}

/* [FIX-4] Get current verification status (for diagnostics) */
VerificationStatus_t transport_ble_get_verification_status(void)
{
    return g_ble_ctx.verification_result;
}

/* [FIX-4] Check verification result with timeout watchdog.
 * Called from task_session on each tick to monitor verification progress.
 * Returns:
 *   CEEPEW_OK: verification passed or still waiting
 *   CEEPEW_ERR_AUTH_FAIL: peer verification failed (mismatch)
 *   CEEPEW_ERR_HW: verification timeout exceeded */
CeePewErr_t transport_ble_check_verification_result(void)
{
    CEEPEW_ASSERT(s_ble_initialised, CEEPEW_ERR_PARAM);
    
    /* If verification already confirmed, no further checking needed */
    if (g_ble_ctx.commitment_verified) {
        return CEEPEW_OK;
    }
    
    /* If no peer verification in progress, return OK (may not have started yet) */
    if (!g_ble_ctx.peer_verification_pending) {
        return CEEPEW_OK;
    }
    
    /* Check if peer has already responded with result */
    if (g_ble_ctx.peer_verification_result == CEEPEW_VERIFY_OK) {
        /* Peer verified successfully — transition to verified state */
        g_ble_ctx.commitment_verified = true;
        g_ble_ctx.handoff_ready = true;
        g_ble_ctx.peer_verification_pending = false;
        ESP_LOGI(TAG, "Peer verification result: OK");
        return CEEPEW_OK;
    }
    
    if (g_ble_ctx.peer_verification_result == CEEPEW_VERIFY_MISMATCH) {
        /* Peer verification failed — both devices should show error */
        g_ble_ctx.peer_verification_pending = false;
        g_ble_ctx.verify_fail_count++;
        ESP_LOGW(TAG, "Peer verification result: MISMATCH (verify_fail_count=%u)", (unsigned)g_ble_ctx.verify_fail_count);
        return CEEPEW_ERR_AUTH_FAIL;
    }
    
    /* Still waiting for peer result — check timeout */
    if (g_ble_ctx.verification_timeout_ms == 0U) {
        /* Timeout not yet armed; this shouldn't happen but bail gracefully */
        return CEEPEW_OK;
    }
    
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (now_ms >= g_ble_ctx.verification_timeout_ms) {
        /* Timeout exceeded — assume failure and recover */
        g_ble_ctx.peer_verification_pending = false;
        g_ble_ctx.verify_fail_count++;
        ESP_LOGW(TAG, "Verification timeout — peer did not respond (verify_fail_count=%u)", (unsigned)g_ble_ctx.verify_fail_count);
        return CEEPEW_ERR_HW;
    }
    
    /* Still within timeout — continue waiting */
    return CEEPEW_OK;
}

/* Thread-safe helper: compute peer age (accumulated + current delta or last_seen)
 * Returns age in milliseconds. Safe to call from UI/task contexts. */
uint32_t transport_ble_get_peer_age_ms(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t age_ms = 0U;

    /* Copy under lock */
    ble_ctx_lock();
    uint32_t acc_ms = g_ble_ctx.accumulated_conn_ms;
    uint32_t gatt_since = g_ble_ctx.gatt_connected_since_ms;
    uint32_t last_seen = g_ble_ctx.last_seen_ms;
    bool gatt_connected = (g_ble_ctx.gattc_connected || g_ble_ctx.gatts_connected);
    ble_ctx_unlock();

    if (gatt_connected && gatt_since != 0U) {
        uint32_t cur_ms = (now_ms > gatt_since) ? (now_ms - gatt_since) : 0U;
        if (cur_ms > UINT32_MAX - acc_ms) {
            age_ms = UINT32_MAX;
        } else {
            age_ms = acc_ms + cur_ms;
        }
    } else {
        if (acc_ms != 0U) {
            age_ms = acc_ms;
        } else {
            age_ms = (last_seen == 0U) ? 0U : (now_ms - last_seen);
        }
    }

    return age_ms;
}

/* Reset accumulated connected time (called on successful handoff) */
void transport_ble_reset_accumulated_conn_ms(void)
{
    ble_ctx_lock();
    g_ble_ctx.accumulated_conn_ms = 0U;
    ble_ctx_unlock();
}

#endif

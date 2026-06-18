/* components/transport/transport_ble.c
 *
 * BLE Transport Implementation (Bluedroid).
 *
 * This is the ACTIVE BLE transport for Phases 1-2 (Discovery + Pairing).
 * It handles:
 *   - BLE advertisement & scanning (Phase 1 discovery)
 *   - Commitment beacon exchange via scan response (Phase 2 pairing)
 *   - Brief GATT connection for Ed25519 sign_pk delivery (Phase 2)
 *   - BLE teardown and handoff to ESP-NOW (Phase 3 transition)
 *
 * transport_ble_gatt_crypto.c: GATT commitment exchange crypto wrapper (Phase 2).
 *   Provides gatt_crypto_encrypt_with_ids() / gatt_crypto_decrypt_with_ids()
 *   for Ascon-128 AEAD encryption of sign_pk || box_pubkey || wifi_mac payload.
 *   Called by transport_ble.c during GATTC write / GATTS read.
 *
 * After Phase 3 handoff, neither is in the hot path — see transport_espnow.c.
 */

#include "transport_ble.h"
#include "transport_ble_gatt_crypto.h"
#include "session_fsm.h"
#include "session_memory.h"
#include "ui_manager.h"
#include "ceepew_security_utils.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <string.h>
#include <stdio.h>
#include <esp_timer.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_coexist.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
#include "esp_mac.h"
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
static void transport_ble_set_ready_for_chat_unlocked(void);
static CeePewErr_t transport_ble_set_commitment_beacon_unlocked(const uint8_t *commitment, uint8_t len);

static void transport_ble_update_state_from_flags_unlocked(void)
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

/* Service 0xFFF0 advertised in the primary ADV so peers can filter scans.
 * Only one characteristic is exposed: 0xFFF3 (sign_pk) for the brief GATT
 * exchange that follows the beacon commitment match. */
static const uint16_t BLE_SERVICE_UUID       = 0xFFF0;
static const uint16_t BLE_SIGN_PK_CHAR_UUID   = 0xFFF3;
static const uint8_t  s_adv_service_uuid16[2] = {
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
static esp_bt_uuid_t s_sign_pk_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = BLE_SIGN_PK_CHAR_UUID }
};

/* 80-byte attribute value buffer for the sign_pk characteristic.
 * Must accommodate 64B ciphertext + 16B tag = 80B total.
 * Explicit max_length ensures GATT write succeeds for full payload. */
static uint8_t s_sign_pk_attr_val[80] = {0};

static esp_attr_value_t s_sign_pk_attr_val_cfg = {
    .attr_max_len = sizeof(s_sign_pk_attr_val),
    .attr_len = 0,
    .attr_value = s_sign_pk_attr_val
};

static esp_attr_control_t s_sign_pk_attr_control = {
    .auto_rsp = ESP_GATT_AUTO_RSP
};
static esp_bt_uuid_t s_service_uuid_filter = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = BLE_SERVICE_UUID }
};

/* One-shot timer: delays sign_pk GATT write by 50 ms after service discovery
 * to let the BLE stack settle.  Without this, esp_ble_gattc_write_char()
 * fires while the stack is still processing SEARCH_CMPL, causing status=133. */
static esp_timer_handle_t s_sign_pk_delay_timer = NULL;
static void sign_pk_delayed_dispatch(void *arg);
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
#define CEEPEW_SCAN_STUCK_TIMEOUT_MS 15000U
static uint32_t s_last_restart_ms              = 0U;
static uint32_t s_scan_retry_after_ms          = 0U;
static bool     s_scan_requested               = false;
static bool s_adv_data_set                 = false;
static bool s_scan_rsp_set                 = false;
static bool s_adv_starting                 = false;
static bool s_scan_start_failed            = false;

static uint8_t s_pending_scan_rsp_buf[64U];
static uint8_t s_pending_scan_rsp_len = 0U;
static bool    s_pending_scan_rsp_update = false;

static uint8_t s_pending_adv_buf[64U];
static uint8_t s_pending_adv_len = 0U;
static bool    s_pending_adv_update = false;

static bool s_local_mac_overridden         = false; /* set if caller provided local_mac explicitly */
static uint32_t s_last_scan_seen_value         = 0U;
static uint32_t s_last_scan_seen_change_ms     = 0U;

/* Mutex protecting access to g_ble_ctx transient fields that are read/written
 * from BLE callback context and task contexts on different cores. */
static SemaphoreHandle_t s_ble_ctx_mutex = NULL;

/* Deadlock diagnostics: track which task holds the lock and for how long. */
static TaskHandle_t s_ble_ctx_lock_owner = NULL;
static uint32_t     s_ble_ctx_lock_held_since_ms = 0U;
#define BLE_CTX_LOCK_TIMEOUT_MS 2000U

static inline bool ble_ctx_lock(void)
{
    if (s_ble_ctx_mutex == NULL) { return true; }
    TickType_t timeout_ticks = pdMS_TO_TICKS(BLE_CTX_LOCK_TIMEOUT_MS);
    if (xSemaphoreTake(s_ble_ctx_mutex, timeout_ticks) != pdTRUE) {
        /* Log the deadlock but do not crash — return false so callers can
         * abort their operation gracefully instead of proceeding with
         * potentially stale g_ble_ctx state. */
        TaskHandle_t holder = s_ble_ctx_lock_owner;
        const char *holder_name = (holder != NULL) ? pcTaskGetName(holder) : "???";
        ESP_LOGE(TAG, "ble_ctx_lock DEADLOCK: held by %s for >%u ms — aborting",
                 holder_name, (unsigned)BLE_CTX_LOCK_TIMEOUT_MS);
        return false;
    }
    s_ble_ctx_lock_owner = xTaskGetCurrentTaskHandle();
    s_ble_ctx_lock_held_since_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    return true;
}

static inline void ble_ctx_unlock(void)
{
    if (s_ble_ctx_mutex == NULL) { return; }
    s_ble_ctx_lock_owner = NULL;
    s_ble_ctx_lock_held_since_ms = 0U;
    xSemaphoreGive(s_ble_ctx_mutex);
}

/* ========================================================================== */
/* Event-Driven Pairing Architecture (Deterministic, Non-Blocking)             */
/* ========================================================================== */

PairingContext_t s_pairing_ctx = {
    .phase = PAIRING_PHASE_IDLE,
    .phase_entered_ms = 0U,
    .last_event_ms = 0U,
};

QueueHandle_t g_pairing_event_queue = NULL;

static TaskHandle_t s_supervisor_task_handle = NULL;
static volatile uint8_t s_supervisor_running = 0U;
static volatile uint8_t s_supervisor_recovering = 0U;

/* Non-blocking recovery state machine (Phase 7 cleanup).
 * Replaces the vTaskDelay(500)+vTaskDelay(100) calls in
 * transport_ble_handle_event_internal that used to stall the supervisor
 * task for 600ms during forced radio recovery. The supervisor tick
 * (CEEPEW_SUPERVISOR_PERIOD_MS = 500) drives the state machine forward. */
typedef enum {
    RECOVERY_STATE_IDLE = 0U,
    RECOVERY_STATE_VERIFY_RESTART,    /* waiting ~500ms after restart to verify radio */
    RECOVERY_STATE_HARD_RESET_DEINIT, /* deinit done, waiting ~100ms before init */
    RECOVERY_STATE_HARD_RESET_INIT    /* init done; one more tick to settle */
} RecoveryState_t;
static volatile RecoveryState_t s_recovery_state = RECOVERY_STATE_IDLE;
static uint32_t s_recovery_started_ms = 0U;

static void transport_ble_supervisor_task(void *arg);
static void transport_ble_supervisor_check_stall(void);
static void transport_ble_supervisor_tick_recovery(void);
static CeePewErr_t transport_ble_handle_event_internal(const PairingEvent_t *event);
static uint32_t transport_ble_phase_timeout_ms(PairingPhase_t phase);
static const char *transport_ble_phase_name(PairingPhase_t phase);
static const char *transport_ble_event_name(PairingEventType_t type);
static void transport_ble_enter_phase_unlocked(PairingPhase_t phase);

static bool s_ble_initialised = false;
static bool s_stack_needs_full_init = true; /* false after first full init; set true by deinit */

static void transport_ble_log_state_snapshot(const char *prefix)
{
    char mac[18];
    transport_ble_format_mac(g_ble_ctx.peer_mac, mac);
    ESP_LOGI(TAG, "%s: state=%s discovered=%u gattc=%u gatts=%u conn_id=%u local_len=%u peer_pending=%u beacon=%u sign_pk=%u reconnects=%u",
             prefix,
             transport_ble_state_name(g_ble_ctx.state),
             g_ble_ctx.discovered ? 1U : 0U,
             g_ble_ctx.gattc_connected ? 1U : 0U,
             g_ble_ctx.gatts_connected ? 1U : 0U,
             (unsigned)g_ble_ctx.conn_id,
             (unsigned)g_ble_ctx.local_commitment_len,
             g_ble_ctx.peer_commitment_pending ? 1U : 0U,
             g_ble_ctx.commitment_beacon_active ? 1U : 0U,
             g_ble_ctx.sign_pk_received ? 1U : 0U,
             (unsigned)g_ble_ctx.reconnect_attempts);
}

CeePewErr_t transport_ble_restart_discovery_session(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    if (s_last_restart_ms != 0U &&
        (uint32_t)(now_ms - s_last_restart_ms) < CEEPEW_RESTART_DEBOUNCE_MS) {
        ESP_LOGI(TAG, "restart_discovery: debounced (%lu ms since last restart)",
                 (unsigned long)(now_ms - s_last_restart_ms));
        return CEEPEW_OK;
    }
    s_last_restart_ms = now_ms;

    /* Re-initialize BLE if it was deinitialized (Phase 3 exit/recovery) */
    if (!s_ble_initialised) {
        ESP_LOGI(TAG, "restart_discovery: BLE was deinitialized, re-initializing stack");
        CeePewErr_t init_err = transport_ble_init();
        if (init_err != CEEPEW_OK) {
            ESP_LOGE(TAG, "Failed to re-initialize BLE: %d", (int)init_err);
            return init_err;
        }
    }

    uint8_t local_device_id[CEEPEW_DEVICE_ID_BYTES] = {0U};
    esp_err_t err_mac = esp_read_mac(local_device_id, ESP_MAC_BT);
    if (err_mac != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed during pairing reset: %d", (int)err_mac);
        return CEEPEW_ERR_HW;
    }

    CeePewErr_t err = session_phase1_init(local_device_id);
    if (err != CEEPEW_OK) {
        ESP_LOGW(TAG, "session_restart_discovery failed during pairing reset: %d", (int)err);
        return err;
    }

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
    ESP_LOGI(TAG, "transport_ble: discovery restarted (adv + scan requested)");

    return CEEPEW_OK;
}

/* Design note: The BLE transport uses Bluedroid (ESP-IDF's built-in BLE stack).
   Phase 1 uses advertisements for discovery.  Phase 2 uses the beacon-encoded
   commitment in SCAN_RSP plus a brief GATT exchange for sign_pk.  Once the
   commitment is verified and handoff_ready is set, the session moves to
   Phase 3 over ESP-NOW and BLE is released.  This keeps BLE usage minimal
   (no bulk data transfer) and reserves bandwidth for ESP-NOW's higher
   throughput. */

BleContext_t g_ble_ctx = {0};

static bool s_scan_peer_dedupe_valid = false;
static uint8_t s_scan_peer_dedupe_mac[6U] = {0U};
static uint32_t s_scan_peer_dedupe_seen_ms = 0U;

static void transport_ble_clear_discovery_peer_state_unlocked(void)
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
    g_ble_ctx.pending_peer_commitment_len = 0U;
    g_ble_ctx.peer_commitment_pending = false;
    ceepew_secure_zero(g_ble_ctx.pending_peer_commitment,
                       (uint32_t)sizeof(g_ble_ctx.pending_peer_commitment));
    g_ble_ctx.commitment_beacon_active = false;
    g_ble_ctx.peer_commitment_via_adv  = false;
    g_ble_ctx.sign_pk_received         = false;
    g_ble_ctx.box_pubkey_received      = false;
    ceepew_secure_zero(g_ble_ctx.adv_commitment, sizeof(g_ble_ctx.adv_commitment));
    g_ble_ctx.gatts_sign_pk_char_handle = 0U;
    g_ble_ctx.gattc_sign_pk_char_handle = 0U;
    g_ble_ctx.reconnect_attempts   = 0U;
    g_ble_ctx.connecting           = false;
    g_ble_ctx.is_initiator_role    = false;
    g_ble_ctx.beacon_nonce_local        = 0U;  /* first encode will set to 1 */
    g_ble_ctx.beacon_nonce_peer_counter_max = 0U;
    g_ble_ctx.peer_gatt_ready            = false;
    g_ble_ctx.gattc_mtu                  = 23U;   /* BLE 4.0 default ATT MTU */
    g_ble_ctx.gattc_sign_pk_mtu_negotiated = false;
    g_ble_ctx.gattc_sign_pk_write_pending = false;
    g_ble_ctx.reverse_gattc_pending       = false;
    g_ble_ctx.initiator_sign_pk_sent      = false;
    /* GATTS MTU tracking (for responder sign_pk receive) */
    g_ble_ctx.gatts_mtu                  = 23U;   /* BLE 4.0 default ATT MTU */
    g_ble_ctx.gatts_sign_pk_mtu_negotiated = false;
    memset(&g_ble_ctx.peer_record, 0U, sizeof(g_ble_ctx.peer_record));
    s_scan_peer_dedupe_valid = false;
    memset(s_scan_peer_dedupe_mac, 0U, sizeof(s_scan_peer_dedupe_mac));
    s_scan_peer_dedupe_seen_ms = 0U;

    s_pending_scan_rsp_update = false;
    memset(s_pending_scan_rsp_buf, 0U, sizeof(s_pending_scan_rsp_buf));
    s_pending_scan_rsp_len = 0U;
}

void transport_ble_clear_discovery_peer_state(void)
{
    ble_ctx_lock();
    transport_ble_clear_discovery_peer_state_unlocked();
    ble_ctx_unlock();
}

void transport_ble_clear_pending_commitment(void)
{
    ble_ctx_lock();
    g_ble_ctx.peer_commitment_pending = false;
    g_ble_ctx.pending_peer_commitment_len = 0U;
    ceepew_secure_zero(g_ble_ctx.pending_peer_commitment,
                       (uint32_t)sizeof(g_ble_ctx.pending_peer_commitment));
    ble_ctx_unlock();
}

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

static const char *transport_ble_hci_reason_name(uint8_t reason)
{
    switch (reason) {
        case 0x05: return "AUTH_FAILURE";
        case 0x06: return "PIN_OR_KEY_MISSING";
        case 0x08: return "CONNECTION_TIMEOUT";
        case 0x13: return "REMOTE_USER_TERMINATED";
        case 0x14: return "REMOTE_LOW_RESOURCES";
        case 0x15: return "REMOTE_POWER_OFF";
        case 0x16: return "LOCAL_HOST_TERMINATED";
        case 0x1A: return "UNSUPPORTED_REMOTE_FEATURE";
        case 0x22: return "LMP_RESPONSE_TIMEOUT";
        case 0x28: return "INSTANT_PASSED";
        case 0x3B: return "LOCAL_HOST_TERMINATED_CONN";
        case 0x3E: return "CONN_FAILED_ESTABLISHMENT";
        default:   return "UNKNOWN";
    }
}

static bool transport_ble_local_commitment_ready(void)
{
    if (g_ble_ctx.local_commitment_len != CEEPEW_COMMITMENT_BYTES) {
        return false;
    }

    uint8_t nonzero = 0U;
    for (uint8_t i = 0U; i < g_ble_ctx.local_commitment_len; i++) {
        nonzero |= g_ble_ctx.commitment_digest[i];
    }
    return nonzero != 0U;
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

/*
 * Encode `len` bytes of `commitment` into the BLE scan-response payload as
 * a manufacturer-specific AD record (company 0xCEEE, subtype 0x50, plus the
 * complete local name "CEEPEW").  The existing 0xFFF0 service UUID in the
 * primary advertisement is left untouched.
 *
 * Layout (31 bytes total for len=16, Bug 2 fix with replay nonce,
 * Phase 7 with GATT-ready flag in bit 15):
 *   [7, 0x09, 'C','E','E','P','E','W']                          — name (8B)
 *   [22, 0xFF, 0xEE, 0xCE, 0x50, nonce_hi, nonce_lo, c0..c15]  — mfr (23B)
 *
 * The 2-byte nonce (big-endian) is monotonic across rebroadcasts. The
 * receiver rejects any beacon whose nonce is <= the highest nonce it
 * has previously accepted — defeating the "captured beacon replay" attack
 * where a third device rebroadcasts a stale commitment to claim a match.
 *
 * Bit 15 of the nonce carries the "GATT-ready" flag (Hybrid-GATT, Phase 7).
 * When our local commitment_verified transitions 0→1, we re-broadcast
 * with bit 15 set; the peer's beacon decoder stores the flag in
 * g_ble_ctx.peer_gatt_ready, unblocking the initiator's GATTC connect
 * gate. The wire value (counter | flag<<15) is always strictly
 * increasing because:
 *   - the counter increments on every call
 *   - the flag going 0→1 increases the high bit (no-op to monotonicity)
 *   - the flag going 1→0 only happens on session reset, which also
 *     resets seen_max, so no false rejection.
 *
 * SCAN_RSP budget: 8 (name) + 1+1+2+1+2+16 = 31 bytes — exactly at limit.
 *
 * Company ID 0xCEEE is unregistered with the Bluetooth SIG and used only for
 * internal CEE-PEW pairing discovery.  Production deployments should
 * register a real company ID with the SIG.
 */
static CeePewErr_t transport_ble_configure_data_raw_unlocked(const uint8_t *adv_data, uint8_t adv_len,
                                                             const uint8_t *scan_rsp_data, uint8_t scan_rsp_len)
{
#ifdef CONFIG_BT_ENABLED
    if (g_ble_ctx.is_advertising) {
        if (adv_data && adv_len > 0) {
            memcpy(s_pending_adv_buf, adv_data, adv_len);
            s_pending_adv_len = adv_len;
            s_pending_adv_update = true;
        } else {
            s_pending_adv_update = false;
        }

        if (scan_rsp_data && scan_rsp_len > 0) {
            memcpy(s_pending_scan_rsp_buf, scan_rsp_data, scan_rsp_len);
            s_pending_scan_rsp_len = scan_rsp_len;
            s_pending_scan_rsp_update = true;
        } else {
            s_pending_scan_rsp_update = false;
        }

        ESP_LOGI(TAG, "configure_data_raw: advertising active — stopping first to apply new payloads");
        esp_err_t err = esp_ble_gap_stop_advertising();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ble_gap_stop_advertising failed: %d", err);
            s_pending_adv_update = false;
            s_pending_scan_rsp_update = false;
            if (adv_data && adv_len > 0) {
                esp_ble_gap_config_adv_data_raw((uint8_t *)adv_data, adv_len);
            }
            if (scan_rsp_data && scan_rsp_len > 0) {
                esp_ble_gap_config_scan_rsp_data_raw((uint8_t *)scan_rsp_data, scan_rsp_len);
            }
        }
    } else {
        ESP_LOGI(TAG, "configure_data_raw: advertising inactive — applying payloads directly");
        if (adv_data && adv_len > 0) {
            s_adv_data_set = false;
            esp_ble_gap_config_adv_data_raw((uint8_t *)adv_data, adv_len);
        }
        if (scan_rsp_data && scan_rsp_len > 0) {
            s_scan_rsp_set = false;
            esp_ble_gap_config_scan_rsp_data_raw((uint8_t *)scan_rsp_data, scan_rsp_len);
        }
    }
#else
    (void)adv_data;
    (void)adv_len;
    (void)scan_rsp_data;
    (void)scan_rsp_len;
#endif
    return CEEPEW_OK;
}

static CeePewErr_t transport_ble_set_commitment_beacon_unlocked(const uint8_t *commitment, uint8_t len)
{
    CEEPEW_ASSERT(commitment != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_COMMITMENT_ADV_BYTES, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_ble_initialised, CEEPEW_ERR_PARAM);

#ifdef CONFIG_BT_ENABLED
    static uint8_t s_commitment_buf[64U];
    uint8_t pos = 0U;

    /* AD: complete local name "CEEPEW" (8 bytes) */
    s_commitment_buf[pos++] = 7U;
    s_commitment_buf[pos++] = 0x09U;
    s_commitment_buf[pos++] = 'C'; s_commitment_buf[pos++] = 'E'; s_commitment_buf[pos++] = 'E';
    s_commitment_buf[pos++] = 'P'; s_commitment_buf[pos++] = 'E'; s_commitment_buf[pos++] = 'W';

    /* AD: manufacturer-specific commitment beacon with replay nonce.
     * Length byte = 4 (mfr-type + company + subtype) + 2 (nonce) + len. */
    const uint8_t mfr_payload_len = (uint8_t)(CEEPEW_BEACON_NONCE_BYTES + len);
    s_commitment_buf[pos++] = (uint8_t)(4U + mfr_payload_len);
    s_commitment_buf[pos++] = 0xFFU;               /* AD type: manufacturer-specific */
    s_commitment_buf[pos++] = 0xEEU;               /* company ID low  (0xCEEE) */
    s_commitment_buf[pos++] = 0xCEU;               /* company ID high */
    s_commitment_buf[pos++] = 0x50U;               /* subtype: CEEPEW commitment beacon */

    /* Monotonic nonce + GATT-ready flag. Bit 15 = commitment_verified.
     * Counter occupies bits 0-14 (15 bits = 32K rebroadcasts max). The
     * counter is stored internally in bits 0-14 only; the flag bit is
     * OR'd into the wire value at transmission time so the internal
     * state is not affected by the flag. */
    /* Avoid 0 as the first value — if both peers start at 0, the receiver
     * would reject the first beacon. Start at 1. Wrap at 0x7FFF back to
     * 1; the flag bit at 0x8000 is reserved and never used by the
     * counter. */
    if (g_ble_ctx.beacon_nonce_local == 0U) { g_ble_ctx.beacon_nonce_local = 1U; }
    g_ble_ctx.beacon_nonce_local++;
    if (g_ble_ctx.beacon_nonce_local > 0x7FFFU) {
        g_ble_ctx.beacon_nonce_local = 1U;   /* wrap, skip 0 */
    }
    uint16_t counter = g_ble_ctx.beacon_nonce_local;
    uint16_t flag    = g_ble_ctx.commitment_verified ? 0x8000U : 0x0000U;
    uint16_t wire_nonce = (uint16_t)(counter | flag);
    s_commitment_buf[pos++] = (uint8_t)(wire_nonce >> 8);       /* nonce big-endian */
    s_commitment_buf[pos++] = (uint8_t)(wire_nonce & 0xFFU);
    memcpy(&s_commitment_buf[pos], commitment, len);
    pos = (uint8_t)(pos + len);

    memcpy(g_ble_ctx.adv_commitment, commitment, len);
    g_ble_ctx.commitment_beacon_active = false;

    /* Ensure we do not exceed the standard BLE advertising payload limit of 31 bytes */
    CEEPEW_ASSERT(pos <= 31U, CEEPEW_ERR_BOUNDS);

    /* Configure scan response data raw, stopping advertising first if needed */
    CeePewErr_t cfg_err = transport_ble_configure_data_raw_unlocked(
        NULL, 0U,
        s_commitment_buf, pos);
    if (cfg_err != CEEPEW_OK) {
        return cfg_err;
    }

    ESP_LOGI(TAG, "Commitment beacon queued (%u bytes, wire_nonce=0x%04x, gatt_ready=%u)",
             (unsigned)len, (unsigned)wire_nonce,
             g_ble_ctx.commitment_verified ? 1U : 0U);
#else
    (void)commitment;
    (void)len;
#endif
    return CEEPEW_OK;
}

CeePewErr_t transport_ble_set_commitment_beacon(const uint8_t *commitment, uint8_t len)
{
    CEEPEW_ASSERT(commitment != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_COMMITMENT_ADV_BYTES, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_ble_initialised, CEEPEW_ERR_PARAM);

    ble_ctx_lock();
    CeePewErr_t err = transport_ble_set_commitment_beacon_unlocked(commitment, len);
    ble_ctx_unlock();
    return err;
}

CeePewErr_t transport_ble_init(void)
{
    CEEPEW_ASSERT(!s_ble_initialised, CEEPEW_ERR_BUSY);

    memset(&g_ble_ctx, 0U, sizeof(BleContext_t));

    /* Populate local_mac from the factory efuse if the caller did not
     * override via transport_ble_set_local_mac(). This is required for
     * the GATT sign_pk encryption key derivation in
     * transport_ble_gatt_crypto.c — the key is bound to (id_self, id_peer)
     * so the local MAC must be known before any GATT write/read. */
    if (!s_local_mac_overridden) {
#ifdef CONFIG_BT_ENABLED
        if (esp_read_mac(g_ble_ctx.local_mac, ESP_MAC_BT) != ESP_OK) {
            ESP_LOGW(TAG, "transport_ble_init: esp_read_mac failed — "
                          "local MAC all zeros, GATT crypto will fail");
        }
#endif
    }
    g_ble_ctx.peer_name_len = 0U;
    g_ble_ctx.peer_rssi = 0;
    g_ble_ctx.peer_rssi_smooth_x8 = 0;
    g_ble_ctx.last_seen_ms = 0U;
    g_ble_ctx.gatt_connected_since_ms = 0U;
    g_ble_ctx.scan_hit_count = 0U;
    g_ble_ctx.scan_seen_count = 0U;
    g_ble_ctx.adv_packet_count = 0U;

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
    g_ble_ctx.gattc_sign_pk_char_handle = 0U;
    g_ble_ctx.gatts_sign_pk_char_handle = 0U;
    g_ble_ctx.gattc_registered = false;
    g_ble_ctx.gatts_registered = false;
    g_ble_ctx.gattc_connected = false;
    g_ble_ctx.gatts_connected = false;
    g_ble_ctx.connecting = false;
    g_ble_ctx.peer_addr_type = BLE_ADDR_TYPE_PUBLIC;
    g_ble_ctx.pending_peer_commitment_len = 0U;
    g_ble_ctx.peer_commitment_pending = false;
    g_ble_ctx.peer_gatt_ready = false;
    g_ble_ctx.gattc_mtu = 23U;                /* BLE 4.0 default ATT MTU */
    g_ble_ctx.gattc_sign_pk_mtu_negotiated = false;
    g_ble_ctx.gattc_sign_pk_write_pending = false;
    g_ble_ctx.pending_sign_pk_write = false;
    memset(g_ble_ctx.pending_sign_pk_encrypted, 0U, sizeof(g_ble_ctx.pending_sign_pk_encrypted));
    memset(g_ble_ctx.pending_peer_commitment, 0U, sizeof(g_ble_ctx.pending_peer_commitment));

    s_scan_requested = false;
    s_adv_data_set = false;
    s_scan_rsp_set = false;
    s_scan_peer_dedupe_valid = false;
    s_scan_peer_dedupe_seen_ms = 0U;
    memset(s_scan_peer_dedupe_mac, 0U, sizeof(s_scan_peer_dedupe_mac));

    ESP_LOGI(TAG, "BLE context reset: state=%s adv=%u scan=%u",
             transport_ble_state_name(g_ble_ctx.state),
             g_ble_ctx.is_advertising ? 1U : 0U,
             g_ble_ctx.is_scanning ? 1U : 0U);

    if (s_ble_ctx_mutex == NULL) {
        s_ble_ctx_mutex = xSemaphoreCreateMutex();
        if (s_ble_ctx_mutex == NULL) {
            ESP_LOGW(TAG, "transport_ble_init: failed to create ble_ctx mutex");
        }
    }

#ifdef CONFIG_BT_ENABLED
    ESP_LOGI(TAG, "transport_ble_init: Starting BLE initialization");

    esp_err_t err;
    if (s_stack_needs_full_init) {
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
        s_stack_needs_full_init = false;

        /* Restore balanced RF coexistence now that BLE is active again.
         * task_session.c sets ESP_COEX_PREFER_WIFI after BLE deinit to
         * ensure ESP-NOW TX succeeds during post-derive sync; reverse
         * that once BLE reclaims the radio. */
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    } else {
        ESP_LOGI(TAG, "BLE stack already initialised — skipping controller+bluedroid init");
    }

    /* Create the delayed sign_pk write timer (one-shot, 50 ms settle).
     * Safe to create even if the timer already exists from a prior init. */
    if (s_sign_pk_delay_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = sign_pk_delayed_dispatch,
            .name     = "sign_pk_delay"
        };
        esp_err_t t_err = esp_timer_create(&timer_args, &s_sign_pk_delay_timer);
        if (t_err != ESP_OK) {
            ESP_LOGE(TAG, "sign_pk delay timer create failed: %d (%s)",
                     (int)t_err, esp_err_to_name(t_err));
            return CEEPEW_ERR_HW;
        }
    }

    /* Set initialization flag BEFORE registering callbacks to avoid race condition. */
    s_ble_initialised = true;

    esp_ble_gap_register_callback(gap_event_handler);
    ESP_LOGI(TAG, "GAP callback registered");

    esp_ble_gattc_register_callback(gattc_event_handler);
    ESP_LOGI(TAG, "GATTC callback registered");

    esp_ble_gatts_register_callback(gatts_event_handler);
    ESP_LOGI(TAG, "GATTS callback registered");

    CeePewErr_t adv_err = transport_ble_start_advertising();
    if (adv_err != CEEPEW_OK) {
        ESP_LOGI(TAG, "Early advertising start returned: %d (will retry on GATT events)", adv_err);
    } else {
        ESP_LOGI(TAG, "Advertising configuration initiated early");
    }

    err = esp_ble_gattc_app_register(0);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "GATTC app already registered (continuing)");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gattc_app_register failed: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }

    err = esp_ble_gatts_app_register(0);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "GATTS app already registered (continuing)");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gatts_app_register failed: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }

    ESP_LOGI(TAG, "transport_ble_init: COMPLETE - BLE ready");
#endif

    CeePewErr_t sup_err = transport_ble_supervisor_start();
    if (sup_err != CEEPEW_OK) {
        ESP_LOGW(TAG, "transport_ble_init: supervisor start failed: %d", (int)sup_err);
    }
    transport_ble_enter_phase(PAIRING_PHASE_IDLE);

    return CEEPEW_OK;
}

void transport_ble_set_local_mac(const uint8_t mac[6])
{
    if (mac == NULL) { return; }
    s_local_mac_overridden = true;
    /* Allow this to be called either before or after transport_ble_init();
     * if before, the value is captured into g_ble_ctx.local_mac by init.
     * If after, the value is updated live. */
    if (s_ble_initialised) {
        memcpy(g_ble_ctx.local_mac, mac, 6U);
    }
}

CeePewErr_t transport_ble_start_advertising(void)
{
    CEEPEW_ASSERT(s_ble_initialised, CEEPEW_ERR_PARAM);
    if (g_ble_ctx.state != BLE_IDLE &&
        g_ble_ctx.state != BLE_ADVERTISING &&
        g_ble_ctx.state != BLE_SCANNING &&
        g_ble_ctx.state != BLE_ADVERTISING_AND_SCANNING) {
        return CEEPEW_ERR_BUSY;
    }
    if (session_is_active()) {
        ESP_LOGI(TAG, "start_advertising: session active — skipping re-advertise");
        return CEEPEW_OK;
    }

#ifdef CONFIG_BT_ENABLED
    CeePewErr_t cfg_err = transport_ble_configure_data_raw_unlocked(
        s_adv_raw_data, sizeof(s_adv_raw_data),
        s_scan_rsp_raw_data, sizeof(s_scan_rsp_raw_data));
    if (cfg_err != CEEPEW_OK) {
        return cfg_err;
    }

    ESP_LOGI(TAG, "Advertisement and scan response configured; awaiting GAP callbacks (svc=0x%04X)",
             (unsigned)BLE_SERVICE_UUID);

#else
#endif

    /* State reflects current conditions (is_advertising may already be true
     * from a prior ADV_START_COMPLETE_EVT).  Do NOT clobber it here. */
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
    ESP_LOGI(TAG, "Scan requested: active=%u interval=%u window=%u duplicate=%u",
             1U, 0x50U, 0x30U, 0U);
    esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x50,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
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
    ESP_LOGI(TAG, "Scan parameters set — waiting for SCAN_PARAM_SET_COMPLETE_EVT");
#endif

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

static CeePewErr_t transport_ble_connect_to_peer_unlocked(const uint8_t peer_mac[6])
{
    if (peer_mac == NULL) { return CEEPEW_ERR_NULL_PTR; }

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
    g_ble_ctx.is_initiator_role = true;
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
#else
    g_ble_ctx.gattc_connected = true;
    g_ble_ctx.is_initiator_role = true;
    g_ble_ctx.state = BLE_CONNECTED;
#endif
    return CEEPEW_OK;
}

CeePewErr_t transport_ble_connect_to_peer(const uint8_t peer_mac[6])
{
    if (!ble_ctx_lock()) {
        ESP_LOGE(TAG, "connect_to_peer: ble_ctx_lock timed out — aborting connect");
        return CEEPEW_ERR_BUSY;
    }
    CeePewErr_t err = transport_ble_connect_to_peer_unlocked(peer_mac);
    ble_ctx_unlock();
    return err;
}

static CeePewErr_t transport_ble_verify_commitment_unlocked(const uint8_t *peer_digest, uint8_t len)
{
    if (peer_digest == NULL) { return CEEPEW_ERR_NULL_PTR; }
    if (session_get_phase() != 2U) {
        if (g_ble_ctx.state != BLE_PAIRING &&
            g_ble_ctx.state != BLE_CONNECTED &&
            g_ble_ctx.state != BLE_ADVERTISING_AND_SCANNING &&
            g_ble_ctx.state != BLE_SCANNING &&
            g_ble_ctx.state != BLE_ADVERTISING) {
            return CEEPEW_ERR_PARAM;
        }
    }
    if (len != CEEPEW_COMMITMENT_BYTES && len != CEEPEW_COMMITMENT_ADV_BYTES) {
        return CEEPEW_ERR_PARAM;
    }

    g_ble_ctx.commitment_verified = true;
    g_ble_ctx.handoff_ready       = true;
    g_ble_ctx.ready_for_chat      = true;
    g_ble_ctx.state               = BLE_DONE;

    CeePewErr_t verr = session_verify_peer_commitment_with_sig(peer_digest, len);
    if (verr != CEEPEW_OK) {
        g_ble_ctx.commitment_verified = false;
        g_ble_ctx.handoff_ready       = false;
        g_ble_ctx.ready_for_chat      = false;
        /* Keep state as BLE_DONE rather than rolling back to prev_state.
         * Rolling back to BLE_ADVERTISING_AND_SCANNING would re-enter
         * discovery while the caller handles the failure. The caller
         * (verify_pending) transitions to UI_STATE_PAIRING_FAILED and
         * calls session_reset_to_discovery which resets state properly. */
        ESP_LOGW(TAG, "commitment verification failed — err=%d", (int)verr);
        return verr;
    }

    /* Hybrid-GATT: beacons matched on both sides. Move into the GATT_IDENTITY
     * phase so the supervisor arms a 2s watchdog. The initiator's task_session
     * gate is responsible for opening the GATTC connection; the responder
     * transitions through this phase on its way to SIGN_PK_EXCHANGE
     * (GATTS_CONNECT_EVT) without opening anything.
     *
     * Re-broadcast our beacon with the GATT-ready flag (bit 15) set so
     * the peer's beacon decoder sets its own peer_gatt_ready. Without
     * this rebroadcast, the initiator's gate would stay closed and the
     * 2s watchdog would fire on both sides. The cached adv_commitment
     * is the 16-byte truncated form (CEEPEW_COMMITMENT_ADV_BYTES). */
    if (g_ble_ctx.commitment_beacon_active) {
        CeePewErr_t reb_err = transport_ble_set_commitment_beacon_unlocked(
            g_ble_ctx.adv_commitment,
            CEEPEW_COMMITMENT_ADV_BYTES);
        if (reb_err != CEEPEW_OK) {
            ESP_LOGW(TAG, "commitment_verified: GATT-ready rebroadcast failed: %d",
                     (int)reb_err);
        }
    }

    transport_ble_enter_phase_unlocked(PAIRING_PHASE_GATT_IDENTITY);
    (void)rgb_set_pattern(transport_ble_phase_to_rgb(PAIRING_PHASE_GATT_IDENTITY));
    ESP_LOGI(TAG, "GATT-ready rebroadcast dispatched, entering GATT_IDENTITY");

    return CEEPEW_OK;
}

CeePewErr_t transport_ble_verify_commitment(const uint8_t *peer_digest, uint8_t len)
{
    ble_ctx_lock();
    CeePewErr_t err = transport_ble_verify_commitment_unlocked(peer_digest, len);
    ble_ctx_unlock();
    return err;
}

static CeePewErr_t transport_ble_verify_pending_commitment_unlocked(void)
{
    if (!g_ble_ctx.peer_commitment_pending) {
        return CEEPEW_OK;
    }
    if (g_ble_ctx.pending_peer_commitment_len == 0U) {
        return CEEPEW_ERR_PARAM;
    }

    if (!transport_ble_local_commitment_ready()) {
        ESP_LOGW(TAG, "verify_pending: local commitment not ready — deferring");
        return CEEPEW_OK;
    }

    uint8_t pending_len = g_ble_ctx.pending_peer_commitment_len;
    g_ble_ctx.peer_commitment_pending = false;
    g_ble_ctx.pending_peer_commitment_len = 0U;

    ESP_LOGI(TAG, "verify_pending: phase=%d pending_len=%u commit=%02X%02X%02X%02X...",
             session_get_phase(), pending_len,
             g_ble_ctx.pending_peer_commitment[0], g_ble_ctx.pending_peer_commitment[1],
             g_ble_ctx.pending_peer_commitment[2], g_ble_ctx.pending_peer_commitment[3]);

    CeePewErr_t err = transport_ble_verify_commitment_unlocked(
        g_ble_ctx.pending_peer_commitment, pending_len);
    ceepew_secure_zero(g_ble_ctx.pending_peer_commitment,
                       (uint32_t)sizeof(g_ble_ctx.pending_peer_commitment));
    if (err == CEEPEW_OK) {
        ESP_LOGI(TAG, "Beacon commitment verification PASSED — handoff ready");
        g_ble_ctx.peer_ready_for_chat = true;
        return CEEPEW_OK;
    }

    ESP_LOGW(TAG, "Beacon commitment verification failed: %d", (int)err);
    transport_ble_log_state_snapshot("Beacon commitment verification failed");

    /* Broadcast a rejection beacon (subtype 0x51) so the peer
     * transitions to PAIRING_FAILED immediately instead of waiting
     * for its own 30-second timeout. The beacon fits in SCAN_RSP
     * alongside the "CEEPEW" name (13 bytes total, well under 31). */
    {
        uint8_t reject_buf[16U];
        uint8_t rpos = 0U;
        /* AD: complete local name "CEEPEW" */
        reject_buf[rpos++] = 7U;
        reject_buf[rpos++] = 0x09U;
        reject_buf[rpos++] = 'C'; reject_buf[rpos++] = 'E'; reject_buf[rpos++] = 'E';
        reject_buf[rpos++] = 'P'; reject_buf[rpos++] = 'E'; reject_buf[rpos++] = 'W';
        /* AD: manufacturer-specific rejection beacon */
        reject_buf[rpos++] = 4U;                /* length: 2 (company) + 1 (subtype) + 1 (reason) */
        reject_buf[rpos++] = 0xFFU;             /* AD type: manufacturer-specific */
        reject_buf[rpos++] = 0xEEU;             /* company ID low  (0xCEEE) */
        reject_buf[rpos++] = 0xCEU;             /* company ID high */
        reject_buf[rpos++] = 0x51U;             /* subtype: rejection beacon */
        reject_buf[rpos++] = (uint8_t)err;      /* rejection reason (CeePewErr_t) */
        (void)transport_ble_configure_data_raw_unlocked(
            NULL, 0U, reject_buf, rpos);
    }

    session_ui_ctx_lock();
    g_ui_ctx.pairing_result_reason = (err == CEEPEW_ERR_AUTH_FAIL)
                                         ? UI_PAIRING_RESULT_COMMITMENT_FAIL
                                         : UI_PAIRING_RESULT_LINK_FAIL;
    g_ui_ctx.transition_ready = true;
    session_ui_ctx_unlock();

    (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);

    return err;
}

CeePewErr_t transport_ble_verify_pending_commitment(void)
{
    ble_ctx_lock();
    CeePewErr_t err = transport_ble_verify_pending_commitment_unlocked();
    ble_ctx_unlock();
    return err;
}

bool transport_ble_handoff_ready(void)
{
    return g_ble_ctx.handoff_ready && g_ble_ctx.commitment_verified;
}

static void transport_ble_set_ready_for_chat_unlocked(void)
{
    g_ble_ctx.ready_for_chat = true;
    g_ble_ctx.handoff_ready = g_ble_ctx.commitment_verified;
    ESP_LOGI(TAG, "Local ready_for_chat set to true");
}

void transport_ble_set_ready_for_chat(void)
{
    ble_ctx_lock();
    transport_ble_set_ready_for_chat_unlocked();
    ble_ctx_unlock();
}

bool transport_ble_peer_ready_for_chat(void)
{
    return g_ble_ctx.peer_ready_for_chat;
}

bool transport_ble_both_ready_for_chat(void)
{
    /* handoff_ready is gated on commitment_verified, so the conjunction
     * reduces to a single check on the local readiness flag. */
    return g_ble_ctx.ready_for_chat && g_ble_ctx.handoff_ready;
}

static CeePewErr_t transport_ble_disconnect_unlocked(void)
{
    if (g_ble_ctx.state == BLE_IDLE) {
        return CEEPEW_OK;
    }

    BleState_t pre_disconnect_state = g_ble_ctx.state;

#ifdef CONFIG_BT_ENABLED
    esp_err_t err = esp_ble_gap_disconnect(g_ble_ctx.peer_mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gap_disconnect returned %d", err);
    }
#endif

    if (s_discovery_restart_pending) {
        g_ble_ctx.state = BLE_ADVERTISING_AND_SCANNING;
    } else {
        transport_ble_update_state_from_flags_unlocked();
    }
    g_ble_ctx.gattc_connected = false;
    g_ble_ctx.gatts_connected = false;
    g_ble_ctx.connecting = false;
    g_ble_ctx.gatt_connected_since_ms = 0U;
    s_scan_requested = false;
    transport_ble_update_state_from_flags_unlocked();

    if (!s_discovery_restart_pending && !g_ble_ctx.commitment_verified && !session_is_active() &&
        pre_disconnect_state != BLE_PAIRING && pre_disconnect_state != BLE_CONNECTED) {
        s_adv_data_set  = false;
        s_scan_rsp_set  = false;
        s_adv_starting  = false;
        (void)transport_ble_start_advertising();
    }

    ESP_LOGI(TAG, "BLE disconnected: state=%s adv=%u scan=%u (session preserved)",
             transport_ble_state_name(g_ble_ctx.state),
             g_ble_ctx.is_advertising ? 1U : 0U,
             g_ble_ctx.is_scanning ? 1U : 0U);

    return CEEPEW_OK;
}

CeePewErr_t transport_ble_disconnect(void)
{
    if (!ble_ctx_lock()) { return CEEPEW_ERR_BUSY; }
    CeePewErr_t err = transport_ble_disconnect_unlocked();
    ble_ctx_unlock();
    return err;
}

CeePewErr_t transport_ble_deinit(void)
{
    CEEPEW_ASSERT(s_ble_initialised, CEEPEW_ERR_PARAM);

    transport_ble_supervisor_stop();
    s_supervisor_recovering = 0U;
    transport_ble_enter_phase(PAIRING_PHASE_IDLE);

#ifdef CONFIG_BT_ENABLED
    esp_err_t err;
    /* Explicitly stop BLE operations before deinit to free the RF
     * for WiFi/ESP-NOW coexistence during Phase 3. */
    err = esp_ble_gap_stop_scanning();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gap_stop_scanning returned %d", err);
    }
    err = esp_ble_gap_stop_advertising();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gap_stop_advertising returned %d", err);
    }
    /* Do NOT call esp_ble_gatts_app_unregister / esp_ble_gattc_app_unregister
     * here.  They internally trigger bta_dm_disable with a 200 ms
     * BTA_DISABLE_DELAY that blocks the BT controller task on CPU0 long
     * enough to trip the Interrupt Watchdog Timer (IWDT).  Since we have
     * already stopped scanning and advertising the link-layer is idle;
     * leaving the app registrations in place is harmless and avoids the
     * crash. */
#endif

    memset(&g_ble_ctx, 0U, sizeof(BleContext_t));
    g_ble_ctx.peer_name_len = 0U;
    g_ble_ctx.peer_rssi = 0;
    s_scan_requested = false;
    s_adv_data_set = false;
    s_scan_rsp_set = false;
    s_adv_starting = false;
    s_scan_start_failed = false;
    s_scan_retry_after_ms = 0U;
    s_pending_adv_update = false;
    s_pending_scan_rsp_update = false;
    s_pending_adv_len = 0U;
    s_pending_scan_rsp_len = 0U;
    memset(s_pending_adv_buf, 0, sizeof(s_pending_adv_buf));
    memset(s_pending_scan_rsp_buf, 0, sizeof(s_pending_scan_rsp_buf));
    s_ble_initialised = false;
    s_stack_needs_full_init = true;
    return CEEPEW_OK;
}

bool transport_ble_is_initialised(void)
{
    return s_ble_initialised;
}

CeePewErr_t transport_ble_set_scan_duty_cycle(uint16_t interval_ms, uint16_t window_ms){
#ifdef CONFIG_BT_ENABLED
    if (!s_ble_initialised) { return CEEPEW_OK; }
    CEEPEW_ASSERT(interval_ms > 0U && window_ms > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(window_ms <= interval_ms, CEEPEW_ERR_PARAM);

    /* Convert ms to BLE scan units: 1 unit = 0.625 ms */
    uint16_t interval_units = (uint16_t)((uint32_t)interval_ms * 1000U / 625U);
    uint16_t window_units   = (uint16_t)((uint32_t)window_ms * 1000U / 625U);

    /* Skip setting params while scan is active — the BLE stack rejects
     * this with "Cmd Disallowed" (0x0C). Params will be applied on the
     * next scan start. */
    if (g_ble_ctx.is_scanning) {
        ESP_LOGI(TAG, "BLE scan duty (deferred): interval=%u ms, window=%u ms (%u%%)",
                 (unsigned)interval_ms, (unsigned)window_ms,
                 (unsigned)((uint32_t)window_ms * 100U / interval_ms));
        return CEEPEW_OK;
    }

    esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = interval_units,
        .scan_window        = window_units,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };

    esp_err_t err = esp_ble_gap_set_scan_params(&scan_params);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_scan_duty_cycle failed: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "BLE scan duty: interval=%u ms, window=%u ms (%u%%)",
             (unsigned)interval_ms, (unsigned)window_ms,
             (unsigned)((uint32_t)window_ms * 100U / interval_ms));
    return CEEPEW_OK;
#else
    (void)interval_ms; (void)window_ms;
    return CEEPEW_OK;
#endif
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

    /* Peer-lost timeout check.
     *
     * CRITICAL FIX:
     * Do NOT clear the discovered peer if:
     *   (a) a GATT connection is active (gattc or gatts side), OR
     *   (b) the commitment has been verified (pairing succeeded), OR
     *   (c) the session is now active (key derivation completed). */
    {
        bool skip_clear = g_ble_ctx.gattc_connected
                       || g_ble_ctx.gatts_connected
                       || g_ble_ctx.commitment_verified
                       || g_ble_ctx.handoff_ready
                       || session_is_active();

        bool in_pairing_flow = (g_ui_ctx.current_state >= UI_STATE_CODE_ENTRY &&
                                g_ui_ctx.current_state <= UI_STATE_PAIRING);
        const uint32_t PEER_LOST_TIMEOUT_MS = in_pairing_flow
                                              ? CEEPEW_PAIRING_PEER_KEEP_MS
                                              : CEEPEW_DISCOVERY_PEER_CLEAR_MS;

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

    /* Scan activity watchdog: detect a stalled scan where the BLE stack
     * reports scanning but no new peers have been observed for an extended
     * period. In that case, restart the discovery session to recover. */
    if (g_ble_ctx.is_scanning ||
        g_ble_ctx.state == BLE_SCANNING ||
        g_ble_ctx.state == BLE_ADVERTISING_AND_SCANNING) {

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
                s_last_scan_seen_change_ms = now_ms;
                s_last_scan_seen_value = seen;
                return CEEPEW_OK;
            }
        }
    }

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

        ble_ctx_lock();
        if (g_ble_ctx.local_commitment_len > 0U) {
            g_ble_ctx.commitment_beacon_active = true;
            ESP_LOGI(TAG, "Commitment beacon now active in scan response");
        } else {
            g_ble_ctx.commitment_beacon_active = false;
        }
        ble_ctx_unlock();

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
            esp_err_t e = esp_ble_gap_start_scanning(0U);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "esp_ble_gap_start_scanning FAILED: %d (%s)",
                         e, esp_err_to_name(e));
                s_scan_requested    = false;
                s_scan_start_failed = true;
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
        transport_ble_update_state_from_flags_unlocked();
        ESP_LOGI(TAG, "State after scan start: %s",
                 transport_ble_state_name(g_ble_ctx.state));
        break;

    /* ── Advertising start confirmed ──────────────────────────────────── */
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        s_adv_starting = false;
        if (!param) { break; }
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            g_ble_ctx.is_advertising = true;
            ESP_LOGI(TAG, "Advertising confirmed active");
        } else {
            ESP_LOGE(TAG, "Advertising start FAILED: status=%d",
                     param->adv_start_cmpl.status);
        }
        transport_ble_update_state_from_flags_unlocked();
        ESP_LOGI(TAG, "State after adv start: %s",
                 transport_ble_state_name(g_ble_ctx.state));
        break;

    /* ── Advertising stop confirmed ───────────────────────────────────── */
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (!param) { break; }
        if (param->adv_stop_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            g_ble_ctx.is_advertising = false;
            ESP_LOGI(TAG, "Advertising confirmed inactive");
        } else {
            ESP_LOGE(TAG, "Advertising stop FAILED: status=%d",
                     param->adv_stop_cmpl.status);
            g_ble_ctx.is_advertising = false;
        }
        transport_ble_update_state_from_flags_unlocked();

        /* Apply any pending payloads now that advertising is stopped */
        if (s_pending_adv_update) {
            s_pending_adv_update = false;
            s_adv_data_set = false;
            ESP_LOGI(TAG, "ADV_STOP_COMPLETE: configuring pending raw adv data (%u bytes)", s_pending_adv_len);
            esp_ble_gap_config_adv_data_raw(s_pending_adv_buf, s_pending_adv_len);
        }
        if (s_pending_scan_rsp_update) {
            s_pending_scan_rsp_update = false;
            s_scan_rsp_set = false;
            ESP_LOGI(TAG, "ADV_STOP_COMPLETE: configuring pending raw scan response data (%u bytes)", s_pending_scan_rsp_len);
            esp_ble_gap_config_scan_rsp_data_raw(s_pending_scan_rsp_buf, s_pending_scan_rsp_len);
        }
        break;

    /* ── Scan result ──────────────────────────────────────────────────── */
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (!param) { break; }
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) { break; }

        /* Filter out our own advertisements to prevent self-discovery and loops.
         * Use the cached BLE MAC from transport_ble_init() rather than calling
         * esp_read_mac() in the GAP callback context where it may be unsafe. */
        if (memcmp(param->scan_rst.bda, g_ble_ctx.local_mac, 6U) == 0) {
            break;
        }

        if (g_ble_ctx.scan_seen_count < UINT32_MAX) { g_ble_ctx.scan_seen_count++; }

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
            if (found_name_len > 15U) {
                found_name_len = 15U;
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

        bool is_ceepew_peer = ((found_name_len >= 6U &&
                                memcmp(found_name, "CEEPEW", 6U) == 0) ||
                              service_uuid_match ||
                              payload_has_ceepew_name);

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        bool in_pairing_flow = (g_ui_ctx.current_state >= UI_STATE_CODE_ENTRY &&
                                g_ui_ctx.current_state <= UI_STATE_PAIRING);
        uint32_t keep_ms = in_pairing_flow ? CEEPEW_PAIRING_PEER_KEEP_MS
                                           : CEEPEW_DISCOVERY_PEER_CLEAR_MS;
        if (g_ble_ctx.discovered &&
            !g_ble_ctx.gattc_connected &&
            !g_ble_ctx.gatts_connected &&
            !g_ble_ctx.commitment_verified &&
            !g_ble_ctx.handoff_ready &&
            !session_is_active() &&
            !transport_ble_peer_is_recent_at(now_ms, keep_ms)) {
            ESP_LOGI(TAG, "Cached peer expired after %lu ms — clearing discovery cache",
                     (unsigned long)(now_ms - g_ble_ctx.last_seen_ms));
            transport_ble_clear_discovery_peer_state();
        }

        bool duplicate_same_peer = false;
        if (is_ceepew_peer &&
            transport_ble_scan_peer_is_duplicate(param->scan_rst.bda, now_ms)) {
            duplicate_same_peer =
                g_ble_ctx.discovered &&
                ceepew_ct_equal(g_ble_ctx.peer_mac, param->scan_rst.bda, 6U);
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

        } else if (ceepew_ct_equal(g_ble_ctx.peer_mac, param->scan_rst.bda, 6U)) {
            g_ble_ctx.peer_rssi = (int8_t)param->scan_rst.rssi;
            g_ble_ctx.peer_rssi_smooth_x8 =
                (int16_t)((6 * g_ble_ctx.peer_rssi_smooth_x8 + 2 * new_rssi_x8) / 8);
            g_ble_ctx.last_seen_ms =
                (uint32_t)(esp_timer_get_time() / 1000LL);
            g_ble_ctx.scan_hit_count++;

            g_ble_ctx.peer_record.rssi = g_ble_ctx.peer_rssi;
            g_ble_ctx.peer_record.seen_at =
                (uint32_t)(esp_timer_get_time() / 1000000LL);
            transport_ble_log_peer_snapshot("Peer updated:", &g_ble_ctx.peer_record);
        }

        /*
         * Commitment beacon extraction — passive, works for both newly discovered
         * and already-cached peers.  Buffer during Phase 1 or 2 (before key
         * derivation).  This allows the faster device to buffer the slower
         * device's beacon even if Phase 2 has not been entered yet on the
         * slower side.  Verification is gated on Phase 2 + local commitment
         * readiness inside transport_ble_verify_pending_commitment_unlocked().
         */
        uint8_t cur_phase = session_get_phase();

        /* Extract manufacturer-specific data once for both beacon types. */
        uint8_t mfr_len = 0U;
        uint8_t *mfr_ptr = NULL;
        if (is_ceepew_peer) {
            mfr_ptr = esp_ble_resolve_adv_data_by_type(
                raw_adv, total_adv_len, 0xFFU, &mfr_len);
        }

        /* Rejection beacon (subtype 0x51): peer rejected our commitment.
         * Always check, regardless of peer_commitment_pending, so the
         * peer immediately transitions to PAIRING_FAILED when we detect
         * a commitment mismatch.  Only valid after codes entered
         * (cur_phase == 2U) and before commitment is verified. */
        if (is_ceepew_peer && cur_phase == 2U &&
            !g_ble_ctx.commitment_verified &&
            mfr_ptr != NULL && mfr_len >= 3U &&
            mfr_ptr[0] == 0xEEU &&
            mfr_ptr[1] == 0xCEU &&
            mfr_ptr[2] == 0x51U)
        {
            ESP_LOGW(TAG, "Rejection beacon received from %s (reason=%d)",
                     found_mac, (int)mfr_ptr[3]);
            session_ui_ctx_lock();
            g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_COMMITMENT_FAIL;
            g_ui_ctx.transition_ready = true;
            session_ui_ctx_unlock();
            (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
            break;  /* done — no need to process further */
        }

        /* Commitment beacon (subtype 0x50): buffer for later
         * verification.  Gated on no pending commitment to avoid
         * overwriting a buffered beacon before it is verified. */
        if (is_ceepew_peer && (cur_phase == 1U || cur_phase == 2U) &&
            (!g_ble_ctx.commitment_verified || !g_ble_ctx.peer_gatt_ready) &&
            !g_ble_ctx.peer_commitment_pending)
        {
            if (mfr_ptr != NULL &&
                mfr_len == (uint8_t)(3U + CEEPEW_BEACON_PAYLOAD_BYTES) &&
                mfr_ptr[0] == 0xEEU &&
                mfr_ptr[1] == 0xCEU &&
                mfr_ptr[2] == 0x50U)
            {
                /* Hybrid-GATT: extract the 2-byte wire nonce. The wire
                 * value is (counter | flag<<15) where bit 15 is the
                 * peer's GATT-ready signal (its commitment_verified
                 * is true). The counter and flag travel together on
                 * the wire so the existing replay defense is preserved
                 * — the receiver compares only the 15-bit counter
                 * (bits 0-14), ignoring the flag bit, so a flag
                 * transition 1→0 after device reset does not cause
                 * false rejection. */
                uint16_t peer_wire = ((uint16_t)mfr_ptr[3] << 8) |
                                     (uint16_t)mfr_ptr[4];
                uint16_t peer_counter = peer_wire & 0x7FFFU;
                bool peer_gatt_ready_now = (peer_wire & 0x8000U) != 0U;

                bool nonce_ok = false;
                bool new_max  = false;
                ble_ctx_lock();
                if (peer_counter > g_ble_ctx.beacon_nonce_peer_counter_max) {
                    g_ble_ctx.beacon_nonce_peer_counter_max = peer_counter;
                    new_max = true;
                    nonce_ok = true;
                    /* Set peer_gatt_ready ONLY when accepting a new
                     * max — this prevents an attacker from spoofing
                     * the flag in a replayed beacon that we would
                     * otherwise reject on counter. */
                    g_ble_ctx.peer_gatt_ready = peer_gatt_ready_now;
                }
                if (nonce_ok && !g_ble_ctx.commitment_verified) {
                    memcpy(g_ble_ctx.pending_peer_commitment,
                           &mfr_ptr[5],
                           CEEPEW_COMMITMENT_ADV_BYTES);
                    g_ble_ctx.pending_peer_commitment_len  = CEEPEW_COMMITMENT_ADV_BYTES;
                    g_ble_ctx.peer_commitment_pending      = true;
                    g_ble_ctx.peer_commitment_via_adv      = true;
                }
                ble_ctx_unlock();

                if (new_max) {
                    ESP_LOGI(TAG,
                             "Commitment beacon received from %s (wire=0x%04x, gatt_ready=%u)",
                             found_mac,
                             (unsigned)peer_wire,
                             peer_gatt_ready_now ? 1U : 0U);
                } else if (peer_counter < g_ble_ctx.beacon_nonce_peer_counter_max) {
                    /* Counter went backwards — genuine replay attack or stale beacon. */
                    ESP_LOGW(TAG, "Beacon replay rejected (peer_counter=0x%04x < seen_max=0x%04x)",
                             (unsigned)peer_counter,
                             (unsigned)g_ble_ctx.beacon_nonce_peer_counter_max);
                }
                /* Equal counter (peer_counter == seen_max) is a normal duplicate
                 * during BLE scanning — the peer re-sends the same beacon on
                 * every SCAN_REQ. Silently ignore without logging. */
            }
        }
    } break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT: {
        if (!param) { break; }
        if (param->update_conn_params.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Conn params updated: int_min=%d int_max=%d latency=%d timeout=%d",
                     param->update_conn_params.min_int, param->update_conn_params.max_int,
                     param->update_conn_params.latency, param->update_conn_params.timeout);
        } else {
            ESP_LOGW(TAG, "Conn param update FAILED: status=%d",
                     param->update_conn_params.status);
        }
    } break;

    default:
        break;
    }
}

/* ── sign_pk delayed-write timer callback ────────────────────────────────
 * Fires 50 ms after SEARCH_CMPL_EVT or CFG_MTU_EVT to let the BLE stack
 * settle before esp_ble_gattc_write_char().  Without this delay, the write
 * fires while the stack is still processing the prior control-plane event,
 * resulting in ESP_GATT_INTERNAL_ERROR (status=133). */
static void sign_pk_delayed_dispatch(void *arg)
{
    (void)arg;
    if (!g_ble_ctx.pending_sign_pk_write || !g_ble_ctx.gattc_connected) {
        return;
    }
    if (g_ble_ctx.gattc_mtu < 84U) {
        ESP_LOGW(TAG, "delayed dispatch: MTU %u < 84, aborting",
                 (unsigned)g_ble_ctx.gattc_mtu);
        ceepew_secure_zero(g_ble_ctx.pending_sign_pk_encrypted,
                           sizeof(g_ble_ctx.pending_sign_pk_encrypted));
        g_ble_ctx.pending_sign_pk_write = false;
        return;
    }
    ESP_LOGI(TAG, "Delayed sign_pk write dispatch (50 ms settle)");
    esp_err_t wr_err = esp_ble_gattc_write_char(
        g_ble_ctx.gattc_if,
        g_ble_ctx.conn_id,
        g_ble_ctx.gattc_sign_pk_char_handle,
        (uint16_t)sizeof(g_ble_ctx.pending_sign_pk_encrypted),
        g_ble_ctx.pending_sign_pk_encrypted,
        ESP_GATT_WRITE_TYPE_RSP,
        ESP_GATT_AUTH_REQ_NONE);
    if (wr_err != ESP_OK) {
        ESP_LOGW(TAG, "delayed sign_pk write failed: %d (%s)",
                 (int)wr_err, esp_err_to_name(wr_err));
        ceepew_secure_zero(g_ble_ctx.pending_sign_pk_encrypted,
                           sizeof(g_ble_ctx.pending_sign_pk_encrypted));
        g_ble_ctx.pending_sign_pk_write = false;
        (void)transport_ble_disconnect();
    } else {
        g_ble_ctx.gattc_sign_pk_write_pending = true;
        ESP_LOGI(TAG, "delayed sign_pk write dispatched: %u bytes (mtu=%u)",
                 (unsigned)sizeof(g_ble_ctx.pending_sign_pk_encrypted),
                 (unsigned)g_ble_ctx.gattc_mtu);
        ceepew_secure_zero(g_ble_ctx.pending_sign_pk_encrypted,
                           sizeof(g_ble_ctx.pending_sign_pk_encrypted));
        g_ble_ctx.pending_sign_pk_write = false;
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
            (void)transport_ble_start_scan();
            break;

        case ESP_GATTC_OPEN_EVT:
            ESP_LOGI(TAG, "GATTC_OPEN_EVT: status=%d conn_id=%u "
                      "is_init=%d rev_pend=%d init_sent=%d",
                     param->open.status, param->open.conn_id,
                     g_ble_ctx.is_initiator_role,
                     g_ble_ctx.reverse_gattc_pending,
                     g_ble_ctx.initiator_sign_pk_sent);
            if (param->open.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "GATTC open failed: %d", param->open.status);
                g_ble_ctx.connecting = false;
            } else {
                /* Hybrid-GATT: request 247-byte ATT MTU upfront so the
                 * 48B Ascon-encrypted sign_pk fits in a single write.
                 * If negotiation lands below 84 (64B ct + 16B tag + 4B
                 * write overhead), the write will fail and we will
                 * bump reconnect_attempts in the SEARCH_CMPL_EVT branch. */
                esp_err_t mtu_err = esp_ble_gattc_send_mtu_req(
                    gattc_if, param->open.conn_id);
                if (mtu_err != ESP_OK) {
                    ESP_LOGW(TAG, "GATTC MTU request failed: %d (%s)",
                             (int)mtu_err, esp_err_to_name(mtu_err));
                }
            }
            break;

        case ESP_GATTC_CFG_MTU_EVT:
            if (param->cfg_mtu.status == ESP_GATT_OK) {
                g_ble_ctx.gattc_mtu = param->cfg_mtu.mtu;
                g_ble_ctx.gattc_sign_pk_mtu_negotiated = true;
                ESP_LOGI(TAG, "GATTC MTU negotiated: %u (need >= 84 for sign_pk+box_pk)",
                         (unsigned)g_ble_ctx.gattc_mtu);
                if (g_ble_ctx.gattc_mtu < 84U) {
                    ESP_LOGW(TAG, "MTU %u below sign_pk+box_pk threshold — write will be rejected",
                             (unsigned)g_ble_ctx.gattc_mtu);
                    if (g_ble_ctx.pending_sign_pk_write) {
                        ESP_LOGW(TAG, "Discarding buffered sign_pk write (MTU too small)");
                        ceepew_secure_zero(g_ble_ctx.pending_sign_pk_encrypted,
                                           sizeof(g_ble_ctx.pending_sign_pk_encrypted));
                        g_ble_ctx.pending_sign_pk_write = false;
                        (void)transport_ble_disconnect();
                    }
                } else if (g_ble_ctx.pending_sign_pk_write) {
                    /* MTU is confirmed sufficient — buffer and dispatch after
                     * a 50 ms settle delay for consistency with the
                     * SEARCH_CMPL path.  Guard on gattc_connected to avoid
                     * ESP_ERR_INVALID_STATE when the connection has already
                     * been torn down. */
                    if (!g_ble_ctx.gattc_connected) {
                        ESP_LOGW(TAG, "Deferred sign_pk write: connection already gone, discarding");
                        ceepew_secure_zero(g_ble_ctx.pending_sign_pk_encrypted,
                                           sizeof(g_ble_ctx.pending_sign_pk_encrypted));
                        g_ble_ctx.pending_sign_pk_write = false;
                    } else {
                        ESP_LOGI(TAG, "MTU %u OK — buffering sign_pk for 50 ms settle",
                                 (unsigned)g_ble_ctx.gattc_mtu);
                        (void)esp_timer_start_once(s_sign_pk_delay_timer, 50000);
                    }
                }
            } else {
                ESP_LOGW(TAG, "GATTC MTU negotiation failed: status=%d",
                         (int)param->cfg_mtu.status);
                if (g_ble_ctx.pending_sign_pk_write) {
                    ESP_LOGW(TAG, "MTU negotiation failed — discarding buffered sign_pk write");
                    ceepew_secure_zero(g_ble_ctx.pending_sign_pk_encrypted,
                                       sizeof(g_ble_ctx.pending_sign_pk_encrypted));
                    g_ble_ctx.pending_sign_pk_write = false;
                    (void)transport_ble_disconnect();
                }
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
            ESP_LOGI(TAG, "GATTC connected: conn_id=%u retries=%u",
                     (unsigned)g_ble_ctx.conn_id,
                     (unsigned)g_ble_ctx.reconnect_attempts);
            /* NOTE: conn param update intentionally omitted — the brief
             * GATT exchange (connect → write 80B → disconnect) is too short
             * for param negotiation; firing esp_ble_gap_update_conn_params()
             * concurrently with GATT service discovery caused status=133. */
            (void)esp_ble_gattc_search_service(gattc_if, g_ble_ctx.conn_id,
                                               &s_service_uuid_filter);
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
                s_sign_pk_char_uuid,
                &char_elem,
                &count);
            if (err != ESP_OK || count == 0U) {
                ESP_LOGW(TAG, "GATTC sign_pk characteristic not found — aborting sign_pk exchange");
                (void)transport_ble_disconnect();
                break;
            }
            g_ble_ctx.gattc_sign_pk_char_handle = char_elem.char_handle;
            ESP_LOGI(TAG, "GATTC sign_pk characteristic ready: handle=%u",
                     (unsigned)g_ble_ctx.gattc_sign_pk_char_handle);

            /* Hybrid-GATT: encrypt the local sign_pk+box_pubkey under a key derived
             * from the session_code (shared with peer) + sorted device IDs,
             * then write the 80B (64B ct + 16B tag) payload to 0xFFF3.
             *
             * The 64B plaintext is the local Ed25519 sign_pk + X25519 box_pubkey
             * generated at session_phase2_initiate. The 16B tag authenticates the
             * payload — a hostile GATTS device cannot substitute a
             * different key without knowing the session_code. */
            /* Phase guard: if session was reset to discovery (phase 1)
             * while a stale GATT connection was still completing service
             * discovery, sign_pk access would assert.  Disconnect
             * gracefully instead. */
            if (session_get_phase() < 2U) {
                ESP_LOGW(TAG, "GATTC SEARCH_CMPL: phase=%u (< 2) — "
                              "stale connection, disconnecting",
                         (unsigned)session_get_phase());
                (void)transport_ble_disconnect();
                break;
            }
            uint8_t sign_pk[CEEPEW_ED25519_PUBKEY_BYTES];
            CeePewErr_t pk_err = session_get_local_sign_pk(sign_pk);
            if (pk_err != CEEPEW_OK) {
                ESP_LOGE(TAG, "GATTC: local sign_pk unavailable: %d", (int)pk_err);
                ceepew_secure_zero(sign_pk, sizeof(sign_pk));
                (void)transport_ble_disconnect();
                break;
            }

            uint8_t box_pk[32];
            CeePewErr_t bp_err = session_get_local_box_pubkey(box_pk);
            if (bp_err != CEEPEW_OK) {
                ESP_LOGE(TAG, "GATTC: local box_pubkey unavailable: %d", (int)bp_err);
                ceepew_secure_zero(sign_pk, sizeof(sign_pk));
                ceepew_secure_zero(box_pk, sizeof(box_pk));
                (void)transport_ble_disconnect();
                break;
            }

            uint8_t session_code[32];
            CeePewErr_t sc_err = session_get_session_code(session_code);
            if (sc_err != CEEPEW_OK) {
                ESP_LOGE(TAG, "GATTC: session_code unavailable: %d", (int)sc_err);
                ceepew_secure_zero(sign_pk, sizeof(sign_pk));
                ceepew_secure_zero(box_pk, sizeof(box_pk));
                ceepew_secure_zero(session_code, sizeof(session_code));
                (void)transport_ble_disconnect();
                break;
            }

            /* Plaintext = sign_pk[32] || box_pubkey[32] || wifi_mac[6] = 70 bytes.
             * Encrypted output = 70B ct + 16B tag = 86 bytes. */
            uint8_t plaintext[GATT_PLAINTEXT_BYTES];
            memcpy(plaintext, sign_pk, CEEPEW_ED25519_PUBKEY_BYTES);
            memcpy(plaintext + CEEPEW_ED25519_PUBKEY_BYTES, box_pk, 32U);
            /* Append local WiFi STA MAC for ESP-NOW peer registration */
            uint8_t wifi_mac[6];
            esp_err_t mac_err = esp_read_mac(wifi_mac, ESP_MAC_WIFI_STA);
            if (mac_err != ESP_OK) {
                ESP_LOGE(TAG, "esp_read_mac(ESP_MAC_WIFI_STA) failed: %d", mac_err);
                ceepew_secure_zero(sign_pk, sizeof(sign_pk));
                ceepew_secure_zero(box_pk, sizeof(box_pk));
                ceepew_secure_zero(session_code, sizeof(session_code));
                (void)transport_ble_disconnect();
                break;
            }
            memcpy(plaintext + 64U, wifi_mac, 6U);
            ceepew_secure_zero(sign_pk, sizeof(sign_pk));
            ceepew_secure_zero(box_pk, sizeof(box_pk));
            ceepew_secure_zero(wifi_mac, sizeof(wifi_mac));

            uint8_t encrypted[GATT_CRYPTO_TOTAL_BYTES];
            CeePewErr_t enc_err = gatt_crypto_encrypt_with_ids(
                session_code,
                g_ble_ctx.local_mac,
                g_ble_ctx.peer_mac,
                plaintext,
                encrypted);
            ceepew_secure_zero(plaintext, sizeof(plaintext));
            ceepew_secure_zero(session_code, sizeof(session_code));
            if (enc_err != CEEPEW_OK) {
                ESP_LOGE(TAG, "GATTC: sign_pk+box_pk encryption failed: %d", (int)enc_err);
                ceepew_secure_zero(encrypted, sizeof(encrypted));
                (void)transport_ble_disconnect();
                break;
            }

            /* Gate sign_pk write on MTU negotiation — the 86-byte
             * payload requires MTU >= 90. If MTU is not yet negotiated,
             * buffer the encrypted payload and dispatch it from the
             * CFG_MTU_EVT handler once the MTU is confirmed sufficient.
             * If MTU IS already negotiated, dispatch immediately but only
             * if the connection is still active (gattc_connected guard). */
            if (!g_ble_ctx.gattc_sign_pk_mtu_negotiated) {
                ESP_LOGI(TAG, "MTU not yet negotiated — buffering sign_pk write for CFG_MTU_EVT");
                memcpy(g_ble_ctx.pending_sign_pk_encrypted, encrypted,
                       sizeof(g_ble_ctx.pending_sign_pk_encrypted));
                g_ble_ctx.pending_sign_pk_write = true;
                ceepew_secure_zero(encrypted, sizeof(encrypted));
            } else if (g_ble_ctx.gattc_mtu < 90U) {
                ESP_LOGW(TAG, "MTU %u < 90 — cannot send sign_pk, aborting",
                         (unsigned)g_ble_ctx.gattc_mtu);
                ceepew_secure_zero(encrypted, sizeof(encrypted));
                (void)transport_ble_disconnect();
            } else if (!g_ble_ctx.gattc_connected) {
                ESP_LOGW(TAG, "GATTC sign_pk: connection gone before write, aborting");
                ceepew_secure_zero(encrypted, sizeof(encrypted));
                (void)transport_ble_disconnect();
            } else {
                /* MTU already negotiated — buffer and dispatch after a 50 ms
                 * settle delay to avoid status=133 from the BLE stack still
                 * processing SEARCH_CMPL.  The timer callback handles the
                 * actual esp_ble_gattc_write_char(). */
                memcpy(g_ble_ctx.pending_sign_pk_encrypted, encrypted,
                       sizeof(g_ble_ctx.pending_sign_pk_encrypted));
                g_ble_ctx.pending_sign_pk_write = true;
                ceepew_secure_zero(encrypted, sizeof(encrypted));
                ESP_LOGI(TAG, "MTU ready — buffering sign_pk for 50 ms settle");
                (void)esp_timer_start_once(s_sign_pk_delay_timer, 50000);
            }
        } break;

        case ESP_GATTC_WRITE_CHAR_EVT: {
            ESP_LOGI(TAG, "GATTC_WRITE_CHAR_EVT: status=%d handle=%u",
                     param->write.status, (unsigned)param->write.handle);
            g_ble_ctx.gattc_sign_pk_write_pending = false;

            if (param->write.status == ESP_GATT_OK) {
                ESP_LOGI(TAG, "GATTC sign_pk write complete — closing link");
                ESP_LOGI(TAG, "reverse_gattc_pending: 1->0 (GATTC write OK, init_sent=%d)",
                         g_ble_ctx.initiator_sign_pk_sent);
                g_ble_ctx.initiator_sign_pk_sent = true;
                g_ble_ctx.reverse_gattc_pending  = false;
                (void)transport_ble_disconnect();
            } else if (param->write.status == 133) {
                /* Status 133 = ESP_GATT_INTERNAL_ERROR — may be
                 * transient (BLE stack contention) or systematic
                 * (cross-connection deadlock).  Treat the first 2 as
                 * genuinely transient; after that, count them toward
                 * the 5-attempt limit so degraded-mode fallthrough
                 * eventually fires. */
                if (g_ble_ctx.reconnect_attempts >= 2U &&
                    g_ble_ctx.reconnect_attempts < CEEPEW_MAX_RECONNECT_ATTEMPTS) {
                    g_ble_ctx.reconnect_attempts++;
                }
                ESP_LOGW(TAG, "GATTC sign_pk write status=133 (retries=%u) — "
                              "keeping rev_pend, will retry",
                         (unsigned)g_ble_ctx.reconnect_attempts);
                (void)transport_ble_disconnect();
            } else {
                ESP_LOGW(TAG, "GATTC sign_pk write failed: status=%d — closing link, "
                              "bumping reconnect_attempts",
                         (int)param->write.status);
                if (g_ble_ctx.reconnect_attempts < CEEPEW_MAX_RECONNECT_ATTEMPTS) {
                    g_ble_ctx.reconnect_attempts++;
                }
                ESP_LOGI(TAG, "reverse_gattc_pending: 1->0 (GATTC write FAIL, status=%d)",
                         param->write.status);
                g_ble_ctx.reverse_gattc_pending = false;
                (void)transport_ble_disconnect();
            }
        } break;

        case ESP_GATTC_DISCONNECT_EVT:
            g_ble_ctx.gattc_connected = false;
            g_ble_ctx.connecting = false;
            g_ble_ctx.gattc_sign_pk_char_handle = 0U;
            /* Cancel any pending delayed sign_pk write and clear buffer */
            if (s_sign_pk_delay_timer != NULL) {
                (void)esp_timer_stop(s_sign_pk_delay_timer);
            }
            if (g_ble_ctx.pending_sign_pk_write) {
                ceepew_secure_zero(g_ble_ctx.pending_sign_pk_encrypted,
                                   sizeof(g_ble_ctx.pending_sign_pk_encrypted));
                g_ble_ctx.pending_sign_pk_write = false;
            }
            ESP_LOGI(TAG, "GATTC disconnected: reason=0x%02x (%s) conn_id=%u",
                     param->disconnect.reason,
                     transport_ble_hci_reason_name(param->disconnect.reason),
                     (unsigned)g_ble_ctx.conn_id);
            ESP_LOGI(TAG, "GATTC disconn ctx: sign_pk_rcvd=%d box_pk=%d "
                      "handoff=%d commit=%d init_sent=%d rev_pend=%d retries=%u",
                     g_ble_ctx.sign_pk_received,
                     g_ble_ctx.box_pubkey_received,
                     g_ble_ctx.handoff_ready,
                     g_ble_ctx.commitment_verified,
                     g_ble_ctx.initiator_sign_pk_sent,
                     g_ble_ctx.reverse_gattc_pending,
                     g_ble_ctx.reconnect_attempts);

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

            if (!g_ble_ctx.commitment_verified && !g_ble_ctx.handoff_ready) {
                if (!s_supervisor_recovering) {
                    transport_ble_handle_connection_lost();
                }
            } else {
                g_ble_ctx.state = BLE_IDLE;
                /* Mirror the GATTS disconnect fix: when handoff is ready, keep
                 * the pairing phase at GATT_IDENTITY so the responder can open
                 * a reverse GATTC for sign_pk exchange. */
                if (g_ble_ctx.handoff_ready) {
                    transport_ble_enter_phase(PAIRING_PHASE_GATT_IDENTITY);
                } else {
                    transport_ble_enter_phase(PAIRING_PHASE_IDLE);
                }
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
            (void)esp_ble_gatts_create_service(gatts_if, &s_service_id, 8U);
            break;

        case ESP_GATTS_CREATE_EVT:
            g_ble_ctx.service_start_handle = param->create.service_handle;
            g_ble_ctx.service_end_handle = param->create.service_handle;
            ESP_LOGI(TAG, "GATTS service created: handle=%u",
                     (unsigned)param->create.service_handle);
            (void)esp_ble_gatts_start_service(param->create.service_handle);
            /* Use explicit attribute value with 80-byte buffer for sign_pk characteristic.
             * The old NULL attr_value + NULL attr_control approach used stack defaults
             * which were too small for the 80-byte (64B ct + 16B tag) payload,
             * causing ESP_GATT_INTERNAL_ERROR (133) on GATT writes. */
            (void)esp_ble_gatts_add_char(
                param->create.service_handle,
                &s_sign_pk_char_uuid,
                ESP_GATT_PERM_WRITE,
                ESP_GATT_CHAR_PROP_BIT_WRITE,
                &s_sign_pk_attr_val_cfg,
                &s_sign_pk_attr_control);
            break;

        case ESP_GATTS_ADD_CHAR_EVT:
            if (g_ble_ctx.gatts_sign_pk_char_handle == 0U) {
                g_ble_ctx.gatts_sign_pk_char_handle = param->add_char.attr_handle;
                ESP_LOGI(TAG, "GATTS sign_pk characteristic added: handle=%u",
                         (unsigned)g_ble_ctx.gatts_sign_pk_char_handle);
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
            ESP_LOGI(TAG, "GATTS connected: conn_id=%u state=BLE_CONNECTED",
                     (unsigned)g_ble_ctx.conn_id);
            /* NOTE: conn param update intentionally omitted — same reason
             * as the GATTC side; concurrent L2CAP param negotiation
             * causes status=133 on GATT writes.
             * MTU is negotiated by the GATTC client (initiator); we just
             * receive the negotiated value via ESP_GATTS_MTU_EVT. */
            transport_ble_enter_phase(PAIRING_PHASE_SIGN_PK_EXCHANGE);
            break;

        case ESP_GATTS_MTU_EVT:
            /* MTU negotiation completed (initiated by GATTC client) */
            g_ble_ctx.gatts_mtu = param->mtu.mtu;
            g_ble_ctx.gatts_sign_pk_mtu_negotiated = true;
            ESP_LOGI(TAG, "GATTS MTU negotiated: %u (need >= 84 for sign_pk+box_pk)",
                     (unsigned)g_ble_ctx.gatts_mtu);
            if (g_ble_ctx.gatts_mtu < 84U) {
                ESP_LOGW(TAG, "GATTS MTU %u below sign_pk+box_pk threshold — peer write may fail",
                         (unsigned)g_ble_ctx.gatts_mtu);
            }
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            ble_ctx_lock();
            g_ble_ctx.gatts_connected = false;
            g_ble_ctx.gattc_connected = false;
            g_ble_ctx.connecting      = false;
            ESP_LOGI(TAG, "GATTS disconnect: sign_pk_received=%d box_pk=%d "
                      "handoff=%d commit=%d init_sent=%d rev_pend=%d",
                     g_ble_ctx.sign_pk_received, g_ble_ctx.box_pubkey_received,
                     g_ble_ctx.handoff_ready, g_ble_ctx.commitment_verified,
                     g_ble_ctx.initiator_sign_pk_sent,
                     g_ble_ctx.reverse_gattc_pending);
            if (g_ble_ctx.handoff_ready) {
                ESP_LOGI(TAG, "GATTS disconnect: preserving sign_pk_received (handoff=%d)",
                         g_ble_ctx.sign_pk_received);
            } else {
                g_ble_ctx.sign_pk_received = false;
                g_ble_ctx.box_pubkey_received = false;
            }
            ESP_LOGI(TAG, "GATTS disconnected: reason=0x%02x (%s) conn_id=%u",
                     param->disconnect.reason,
                     transport_ble_hci_reason_name(param->disconnect.reason),
                     (unsigned)g_ble_ctx.conn_id);

           {
               uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
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
           }

           bool pairing_failure_visible_gatts =
               (g_ui_ctx.current_state == UI_STATE_PAIRING_FAILED ||
                g_ui_ctx.next_state == UI_STATE_PAIRING_FAILED);
           if (pairing_failure_visible_gatts) {
               g_ble_ctx.state = BLE_IDLE;
               ble_ctx_unlock();
               break;
           }

            bool should_handle_lost_gatts = (!g_ble_ctx.commitment_verified && !g_ble_ctx.handoff_ready);
            PairingPhase_t gatts_disconnect_phase = PAIRING_PHASE_IDLE;
            bool gatts_disconnect_phase_changed = false;
            if (!should_handle_lost_gatts) {
                g_ble_ctx.state = BLE_IDLE;
                /* When handoff is ready, keep the pairing phase at GATT_IDENTITY
                 * so the initiator can re-open a GATTC to complete the sign_pk
                 * exchange.  Resetting to IDLE here traps the initiator in STEP 5
                 * because its GATTC-open gate requires GATT_IDENTITY.
                 *
                 * NOTE: Use _unlocked variant — we already hold s_ble_ctx_mutex
                 * from line 2273.  Calling transport_ble_enter_phase() (which
                 * re-takes the non-recursive FreeRTOS mutex) caused a self-
                 * deadlock on the BT controller task, permanently blocking all
                 * subsequent ble_ctx_lock() callers including the session task. */
                if (g_ble_ctx.handoff_ready) {
                    gatts_disconnect_phase = PAIRING_PHASE_GATT_IDENTITY;
                } else {
                    gatts_disconnect_phase = PAIRING_PHASE_IDLE;
                }
                transport_ble_enter_phase_unlocked(gatts_disconnect_phase);
                gatts_disconnect_phase_changed = true;
            }
           ble_ctx_unlock();

           /* Update RGB outside the lock — safe (no g_ble_ctx access). */
           if (gatts_disconnect_phase_changed) {
               (void)rgb_set_pattern(transport_ble_phase_to_rgb(gatts_disconnect_phase));
           }

           if (should_handle_lost_gatts && !s_supervisor_recovering) {
               transport_ble_handle_connection_lost();
           }
           break;

        case ESP_GATTS_WRITE_EVT:
            if (param->write.need_rsp) {
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

            if (!param->write.is_prep &&
                param->write.handle == g_ble_ctx.gatts_sign_pk_char_handle) {
                /* Hybrid-GATT (Phase 7): the initiator's sign_pk+box_pubkey is now
                 * wrapped in Ascon-128 AEAD (64B ct + 16B tag = 80B).
                 * The key/nonce are derived from the session_code, so
                 * a hostile GATTS device cannot inject a chosen sign_pk
                 * without knowing the human-typed session code.
                 *
                 * THREAT MODEL:
                 *   - Without session_code, Ascon tag verification fails
                 *     (CEEPEW_ERR_AUTH_FAIL) and the write is silently
                 *     dropped. The peer relationship is severed.
                 *   - The local sign_pk is NEVER sent over BLE — the
                 *     GATTS path remains one-way (peer → us).
                 *
                 * MTU validation: reject writes that don't carry the full
                 * 80-byte encrypted payload (64B ct + 16B tag). A truncated
                 * write would fail Ascon tag verification anyway, but we
                 * check early to get a clearer diagnostic log. */
                if (param->write.len != GATT_CRYPTO_TOTAL_BYTES) {
                    ESP_LOGW(TAG, "GATTS: sign_pk write length %u != %u (MTU %u?) — dropping",
                             (unsigned)param->write.len,
                             (unsigned)GATT_CRYPTO_TOTAL_BYTES,
                             (unsigned)g_ble_ctx.gattc_mtu);
                    break;
                }

                /* The peer's sign_pk+box_pubkey, once decrypted, is used as
                 * the "public key" input to curve25519_scalarmult() in
                 * crypto_box_decrypt(). That operation is symmetric — both
                 * peers do the same multiplication with their own box_seed
                 * and the peer's sign_pk as the point — so the captured
                 * sign_pk alone is insufficient to derive the shared secret.
                 *
                 * Bounded exposure: ephemeral per-session key, brief
                 * GATT window (<= 5 reconnect attempts, ~2s), short
                 * pairing window (30s max per spec), requires
                 * physical proximity. */
                uint8_t session_code[32];
                CeePewErr_t sc_err = session_get_session_code(session_code);
                if (sc_err != CEEPEW_OK) {
                    ESP_LOGE(TAG, "GATTS: session_code unavailable: %d", (int)sc_err);
                    ceepew_secure_zero(session_code, sizeof(session_code));
                    if (g_ble_ctx.reconnect_attempts < CEEPEW_MAX_RECONNECT_ATTEMPTS) {
                        g_ble_ctx.reconnect_attempts++;
                    }
                    break;
                }

                uint8_t decrypted[GATT_PLAINTEXT_BYTES];
                ESP_LOGI(TAG, "GATTS: sign_pk+box_pk write — local=%02X:%02X:%02X:%02X:%02X:%02X "
                         "peer=%02X:%02X:%02X:%02X:%02X:%02X len=%u",
                         g_ble_ctx.local_mac[0], g_ble_ctx.local_mac[1],
                         g_ble_ctx.local_mac[2], g_ble_ctx.local_mac[3],
                         g_ble_ctx.local_mac[4], g_ble_ctx.local_mac[5],
                         g_ble_ctx.peer_mac[0], g_ble_ctx.peer_mac[1],
                         g_ble_ctx.peer_mac[2], g_ble_ctx.peer_mac[3],
                         g_ble_ctx.peer_mac[4], g_ble_ctx.peer_mac[5],
                         (unsigned)param->write.len);
                CeePewErr_t dec_err = gatt_crypto_decrypt_with_ids(
                    session_code,
                    g_ble_ctx.local_mac,
                    g_ble_ctx.peer_mac,
                    param->write.value,
                    decrypted);
                /* Retry with swapped MACs if first attempt fails — handles the
                 * case where local/peer ordering was inverted at encryption time
                 * (e.g. responder encrypts with its own local_mac=peer). */
                if (dec_err == CEEPEW_ERR_AUTH_FAIL || dec_err == CEEPEW_ERR_CRYPTO) {
                    ESP_LOGW(TAG, "GATTS: Ascon decrypt attempt 1 failed (%d) — retrying with swapped MACs",
                             (int)dec_err);
                    uint8_t sc_retry[32];
                    CeePewErr_t sc_retry_err = session_get_session_code(sc_retry);
                    if (sc_retry_err == CEEPEW_OK) {
                        CeePewErr_t dec2 = gatt_crypto_decrypt_with_ids(
                            sc_retry,
                            g_ble_ctx.peer_mac,
                            g_ble_ctx.local_mac,
                            param->write.value,
                            decrypted);
                        ceepew_secure_zero(sc_retry, sizeof(sc_retry));
                        if (dec2 == CEEPEW_OK) {
                            ESP_LOGI(TAG, "GATTS: sign_pk decrypt SUCCEEDED on swapped-MAC retry");
                            dec_err = CEEPEW_OK;
                        } else {
                            ESP_LOGW(TAG, "GATTS: swapped-MAC retry also failed (%d)", (int)dec2);
                        }
                    } else {
                        ceepew_secure_zero(sc_retry, sizeof(sc_retry));
                    }
                }
                ceepew_secure_zero(session_code, sizeof(session_code));
                if (dec_err == CEEPEW_ERR_AUTH_FAIL) {
                    ESP_LOGW(TAG, "GATTS: Ascon tag mismatch on sign_pk write — "
                                  "dropping, bumping reconnect_attempts "
                                  "(local=%02X%02X%02X peer=%02X%02X%02X)",
                             g_ble_ctx.local_mac[3], g_ble_ctx.local_mac[4],
                             g_ble_ctx.local_mac[5],
                             g_ble_ctx.peer_mac[3], g_ble_ctx.peer_mac[4],
                             g_ble_ctx.peer_mac[5]);
                    ceepew_secure_zero(decrypted, sizeof(decrypted));
                    if (g_ble_ctx.reconnect_attempts < CEEPEW_MAX_RECONNECT_ATTEMPTS) {
                        g_ble_ctx.reconnect_attempts++;
                    }
                    transport_ble_post_event(PAIRING_EVENT_PHASE_TIMEOUT, NULL, 0U);
                    break;
                }
                if (dec_err != CEEPEW_OK) {
                    ESP_LOGW(TAG, "GATTS: sign_pk decrypt failed: %d", (int)dec_err);
                    ceepew_secure_zero(decrypted, sizeof(decrypted));
                    if (g_ble_ctx.reconnect_attempts < CEEPEW_MAX_RECONNECT_ATTEMPTS) {
                        g_ble_ctx.reconnect_attempts++;
                    }
                    break;
                }

                CeePewErr_t pk_err = session_set_peer_public_key(decrypted);
                /* Extract and set the peer's box_pubkey (bytes 32-63 of payload).
                 * This enables crypto_box_decrypt() for inbound messages. */
                CeePewErr_t bp_err = session_set_peer_box_pubkey(decrypted + CEEPEW_ED25519_PUBKEY_BYTES);
                /* Extract and set the peer's WiFi STA MAC (bytes 64-69 of payload).
                 * This is used for ESP-NOW peer registration. */
                uint8_t peer_wifi_mac[6];
                memcpy(peer_wifi_mac, decrypted + 64U, 6U);
                CeePewErr_t wifi_mac_err = session_set_peer_wifi_mac(peer_wifi_mac);
                ceepew_secure_zero(peer_wifi_mac, sizeof(peer_wifi_mac));
                ceepew_secure_zero(decrypted, sizeof(decrypted));
                if (pk_err == CEEPEW_OK && wifi_mac_err == CEEPEW_OK) {
                    g_ble_ctx.sign_pk_received = true;
                    g_ble_ctx.box_pubkey_received = (bp_err == CEEPEW_OK);
                    ESP_LOGI(TAG, "GATTS: peer sign_pk SET (sign_pk_rcvd=%d box_pk=%d "
                              "is_init=%d rev_pend=%d init_sent=%d)",
                             g_ble_ctx.sign_pk_received,
                             g_ble_ctx.box_pubkey_received,
                             g_ble_ctx.is_initiator_role,
                             g_ble_ctx.reverse_gattc_pending,
                             g_ble_ctx.initiator_sign_pk_sent);
                    transport_ble_post_event(PAIRING_EVENT_SIGN_PK_RECEIVED,
                                             NULL, 0U);

                    /* Mutual sign_pk exchange (M3): if we are the responder,
                     * schedule a reverse GATTC connection to write our own
                     * sign_pk+box_pk to the initiator's 0xFFF3. */
                    if (!g_ble_ctx.is_initiator_role &&
                        !g_ble_ctx.initiator_sign_pk_sent &&
                        !g_ble_ctx.reverse_gattc_pending) {
                        g_ble_ctx.reverse_gattc_pending = true;
                        ESP_LOGI(TAG, "reverse_gattc_pending: 0->1 (GATTS write received, scheduling reverse)");
                        ESP_LOGI(TAG, "GATTS: scheduling reverse GATTC for sign_pk+box_pk exchange");
                    }
                } else {
                    ESP_LOGW(TAG, "GATTS: session_set_peer_public_key failed: %d",
                             (int)pk_err);
                }
            }
            break;

        default:
            break;
    }
}

/* Thread-safe helper: compute peer age (accumulated + current delta or last_seen)
 * Returns age in milliseconds. Safe to call from UI/task contexts. */
uint32_t transport_ble_get_peer_age_ms(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t age_ms = 0U;

    ble_ctx_lock();
    uint32_t disc_start_ts = g_ble_ctx.discovery_start_ts;
    bool discovered = g_ble_ctx.discovered;
    ble_ctx_unlock();

    if (discovered && disc_start_ts != 0U) {
        age_ms = (now_ms > (disc_start_ts * 1000U)) ? (now_ms - (disc_start_ts * 1000U)) : 0U;
    }

    return age_ms;
}

void transport_ble_reset_accumulated_conn_ms(void)
{
    ble_ctx_lock();
    g_ble_ctx.accumulated_conn_ms = 0U;
    ble_ctx_unlock();
}

/* ========================================================================== */
/* Event-Driven Pairing Architecture Implementation                              */
/* ========================================================================== */

static const char *transport_ble_phase_name(PairingPhase_t phase)
{
    switch (phase) {
        case PAIRING_PHASE_IDLE:             return "IDLE";
        case PAIRING_PHASE_DISCOVERY:        return "DISCOVERY";
        case PAIRING_PHASE_BEACON_PAIRING:   return "BEACON_PAIRING";
        case PAIRING_PHASE_GATT_IDENTITY:    return "GATT_IDENTITY";
        case PAIRING_PHASE_SIGN_PK_EXCHANGE: return "SIGN_PK_EXCHANGE";
        case PAIRING_PHASE_FAILED:           return "FAILED";
        default:                             return "UNKNOWN";
    }
}

static const char *transport_ble_event_name(PairingEventType_t type)
{
    switch (type) {
        case PAIRING_EVENT_NONE:             return "NONE";
        case PAIRING_EVENT_SIGN_PK_RECEIVED: return "SIGN_PK_RECEIVED";
        case PAIRING_EVENT_PHASE_TIMEOUT:    return "PHASE_TIMEOUT";
        case PAIRING_EVENT_RADIO_RESTART:    return "RADIO_RESTART";
        default:                             return "UNKNOWN";
    }
}

static uint32_t transport_ble_phase_timeout_ms(PairingPhase_t phase)
{
    switch (phase) {
        case PAIRING_PHASE_BEACON_PAIRING:   return CEEPEW_PHASE_TIMEOUT_MS;
        case PAIRING_PHASE_GATT_IDENTITY:    return CEEPEW_PHASE_TIMEOUT_GATT_MS;
        case PAIRING_PHASE_SIGN_PK_EXCHANGE: return CEEPEW_PHASE_TIMEOUT_CONNECT_MS;
        default:                             return CEEPEW_PHASE_TIMEOUT_MS;
    }
}

void transport_ble_post_event(PairingEventType_t type,
                              const uint8_t *payload,
                              uint8_t payload_len)
{
    if (g_pairing_event_queue == NULL) {
        return;
    }
    PairingEvent_t ev = {0};
    ev.type = type;
    ev.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    ev.payload_len = (payload != NULL && payload_len <= sizeof(ev.payload))
                     ? payload_len : 0U;
    if (ev.payload_len > 0U) {
        memcpy(ev.payload, payload, ev.payload_len);
    }
    if (xQueueSend(g_pairing_event_queue, &ev, 0U) != pdTRUE) {
        ESP_LOGW(TAG, "pairing event queue full, dropped %s",
                 transport_ble_event_name(type));
    }
}

static void transport_ble_enter_phase_unlocked(PairingPhase_t phase)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (s_pairing_ctx.phase != phase) {
        ESP_LOGI(TAG, "pairing phase: %s -> %s",
                 transport_ble_phase_name(s_pairing_ctx.phase),
                 transport_ble_phase_name(phase));
        s_pairing_ctx.phase = phase;
        s_pairing_ctx.phase_entered_ms = now_ms;
        s_pairing_ctx.last_event_ms = now_ms;
    }
}

void transport_ble_enter_phase(PairingPhase_t phase)
{
    if (!ble_ctx_lock()) {
        ESP_LOGE(TAG, "enter_phase(%s): ble_ctx_lock timed out",
                 transport_ble_phase_name(phase));
        return;
    }
    transport_ble_enter_phase_unlocked(phase);
    ble_ctx_unlock();
    (void)rgb_set_pattern(transport_ble_phase_to_rgb(phase));
}

/* Unified connection-lost handler. Non-blocking.
 * Pushes a phase-timeout event so the supervisor re-evaluates the current
 * pairing phase (the BLE link drop typically means sign_pk GATT failed). */
void transport_ble_handle_connection_lost(void)
{
    ble_ctx_lock();
    g_ble_ctx.gattc_connected = false;
    g_ble_ctx.gatts_connected = false;
    g_ble_ctx.connecting = false;
    ble_ctx_unlock();

    transport_ble_post_event(PAIRING_EVENT_PHASE_TIMEOUT, NULL, 0U);
    ESP_LOGI(TAG, "connection_lost: queued PHASE_TIMEOUT for supervisor re-evaluation");
}

static CeePewErr_t transport_ble_handle_event_internal(const PairingEvent_t *event)
{
    if (event == NULL) { return CEEPEW_ERR_NULL_PTR; }

    switch (event->type) {
        case PAIRING_EVENT_SIGN_PK_RECEIVED: {
            ESP_LOGI(TAG, "supervisor: SIGN_PK_RECEIVED — phase=%s handoff=%d sign_pk=%d",
                     transport_ble_phase_name(s_pairing_ctx.phase),
                     g_ble_ctx.handoff_ready, g_ble_ctx.sign_pk_received);
            break;
        }

        case PAIRING_EVENT_PHASE_TIMEOUT:
        case PAIRING_EVENT_RADIO_RESTART:
            ESP_LOGW(TAG, "supervisor: forced recovery (event=%s)",
                     transport_ble_event_name(event->type));
            s_supervisor_recovering = 1U;
            s_recovery_state = RECOVERY_STATE_VERIFY_RESTART;
            s_recovery_started_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            (void)rgb_set_pattern(RGB_YELLOW_RED_BLINK);
#ifdef CONFIG_BT_ENABLED
            (void)esp_ble_gap_disconnect(g_ble_ctx.peer_mac);
#endif
            (void)transport_ble_clear_discovery_peer_state();
            (void)transport_ble_restart_discovery_session();
            /* Spec §3.2: verify the radio actually came back up after a
             * forced restart. If 500 ms after restart we are still in a
             * non-air state, the radio is wedged — power-cycle it. The
             * actual verification is now performed asynchronously by
             * transport_ble_supervisor_tick_recovery() on subsequent
             * supervisor ticks — no vTaskDelay here. */
            break;

        case PAIRING_EVENT_NONE:
        default:
            break;
    }
    return CEEPEW_OK;
}

static void transport_ble_supervisor_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "pairing supervisor: started");
    while (s_supervisor_running) {
        PairingEvent_t ev = {0};
        BaseType_t got = xQueueReceive(g_pairing_event_queue, &ev,
                                       pdMS_TO_TICKS(CEEPEW_SUPERVISOR_PERIOD_MS));
        if (got == pdTRUE) {
            (void)transport_ble_handle_event_internal(&ev);
        }
        transport_ble_supervisor_check_stall();
        transport_ble_supervisor_tick_recovery();
    }
    ESP_LOGI(TAG, "pairing supervisor: stopped");
    s_supervisor_task_handle = NULL;
    vTaskDelete(NULL);
}

static void transport_ble_supervisor_check_stall(void)
{
    if (s_supervisor_recovering) {
        return;
    }
    ble_ctx_lock();
    PairingPhase_t phase = s_pairing_ctx.phase;
    uint32_t entered = s_pairing_ctx.phase_entered_ms;
    ble_ctx_unlock();

    if (phase == PAIRING_PHASE_IDLE || phase == PAIRING_PHASE_FAILED || phase == PAIRING_PHASE_DISCOVERY) {
        return;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t elapsed = (now_ms >= entered) ? (now_ms - entered) : 0U;
    uint32_t budget = transport_ble_phase_timeout_ms(phase);
    if (elapsed > budget) {
        ESP_LOGW(TAG, "supervisor: phase %s stalled for %u ms (budget %u, entered=%u, now=%u)",
                 transport_ble_phase_name(phase),
                 (unsigned)elapsed, (unsigned)budget,
                 (unsigned)entered, (unsigned)now_ms);
        transport_ble_post_event(PAIRING_EVENT_PHASE_TIMEOUT, NULL, 0U);
    }
}

/* Drive the non-blocking recovery state machine forward. Runs on every
 * supervisor tick (CEEPEW_SUPERVISOR_PERIOD_MS = 500ms). Replaces the
 * vTaskDelay-based recovery path in transport_ble_handle_event_internal
 * with an event-driven design that does not block the supervisor. */
static void transport_ble_supervisor_tick_recovery(void)
{
    if (s_recovery_state == RECOVERY_STATE_IDLE) { return; }
    /* If the supervisor is being torn down, abort any in-flight recovery
     * to prevent it from calling transport_ble_deinit() while the
     * transport_ble_deinit() caller is already running. */
    if (s_supervisor_running == 0U) {
        s_recovery_state = RECOVERY_STATE_IDLE;
        s_supervisor_recovering = 0U;
        return;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t elapsed = (now_ms >= s_recovery_started_ms)
                         ? (now_ms - s_recovery_started_ms) : 0U;

    switch (s_recovery_state) {
        case RECOVERY_STATE_VERIFY_RESTART:
            /* Wait ~500ms after the restart to give the radio time to
             * come back into an air state. If still not on-air, force
             * a hard deinit/init. */
            if (elapsed < 500U) { break; }
            {
                BleState_t post_state = transport_ble_get_state();
                if (post_state == BLE_ADVERTISING ||
                    post_state == BLE_SCANNING ||
                    post_state == BLE_ADVERTISING_AND_SCANNING) {
                    ESP_LOGI(TAG, "supervisor: radio recovered OK (state=%s)",
                             transport_ble_state_name(post_state));
                    s_recovery_state = RECOVERY_STATE_IDLE;
                    s_supervisor_recovering = 0U;
                    transport_ble_enter_phase(PAIRING_PHASE_DISCOVERY);
                } else {
                    ESP_LOGE(TAG,
                             "supervisor: radio failed to restart (state=%s) — "
                             "forcing hard deinit/init",
                             transport_ble_state_name(post_state));
                    s_recovery_state = RECOVERY_STATE_HARD_RESET_DEINIT;
                    s_recovery_started_ms = now_ms;
                    (void)transport_ble_deinit();
                }
            }
            break;

        case RECOVERY_STATE_HARD_RESET_DEINIT:
            /* Wait ~100ms after deinit before re-init. */
            if (elapsed < 100U) { break; }
            s_recovery_state = RECOVERY_STATE_HARD_RESET_INIT;
            s_recovery_started_ms = now_ms;
            (void)transport_ble_init();
            break;

        case RECOVERY_STATE_HARD_RESET_INIT:
            /* One more tick to let the new init settle into a stable
             * state before declaring recovery done. */
            s_recovery_state = RECOVERY_STATE_IDLE;
            s_supervisor_recovering = 0U;
            transport_ble_enter_phase(PAIRING_PHASE_DISCOVERY);
            break;

        case RECOVERY_STATE_IDLE:
        default:
            break;
    }
}

CeePewErr_t transport_ble_supervisor_start(void)
{
    if (s_supervisor_running) { return CEEPEW_OK; }
    if (g_pairing_event_queue == NULL) {
        g_pairing_event_queue = xQueueCreate(CEEPEW_PAIRING_EVENT_QUEUE_DEPTH,
                                             sizeof(PairingEvent_t));
        if (g_pairing_event_queue == NULL) {
            return CEEPEW_ERR_HW;
        }
    }
    s_supervisor_running = 1U;
    BaseType_t ok = xTaskCreate(transport_ble_supervisor_task,
                                "pair_supp",
                                CEEPEW_SUPERVISOR_STACK_BYTES,
                                NULL,
                                CEEPEW_SUPERVISOR_PRIORITY,
                                &s_supervisor_task_handle);
    if (ok != pdPASS) {
        s_supervisor_running = 0U;
        return CEEPEW_ERR_HW;
    }
    return CEEPEW_OK;
}

void transport_ble_supervisor_stop(void)
{
    if (!s_supervisor_running) { return; }
    s_supervisor_running = 0U;
    if (s_supervisor_task_handle != NULL) {
        vTaskDelete(s_supervisor_task_handle);
        s_supervisor_task_handle = NULL;
    }
    if (g_pairing_event_queue != NULL) {
        vQueueDelete(g_pairing_event_queue);
        g_pairing_event_queue = NULL;
    }
}

/* Phase → RGB pattern mapping per spec §4. */
RgbPattern_t transport_ble_phase_to_rgb(PairingPhase_t phase)
{
    switch (phase) {
        case PAIRING_PHASE_IDLE:             return RGB_OFF;
        case PAIRING_PHASE_DISCOVERY:        return RGB_BLUE_PULSE;
        case PAIRING_PHASE_BEACON_PAIRING:   return RGB_AMBER_PULSE;
        case PAIRING_PHASE_GATT_IDENTITY:    return RGB_CYAN_BLINK;
        case PAIRING_PHASE_SIGN_PK_EXCHANGE: return RGB_CYAN_PULSE;
        case PAIRING_PHASE_FAILED:           return RGB_RED_BLINK;
        default:                             return RGB_OFF;
    }
}

PairingPhase_t transport_ble_get_phase(void)
{
    PairingPhase_t phase;
    ble_ctx_lock();
    phase = s_pairing_ctx.phase;
    ble_ctx_unlock();
    return phase;
}

bool transport_ble_is_recovering(void)
{
    return s_supervisor_recovering != 0U;
}

void transport_ble_debug_trigger_timeout(void)
{
    PairingPhase_t phase = transport_ble_get_phase();
    if (phase == PAIRING_PHASE_IDLE || phase == PAIRING_PHASE_FAILED) {
        ESP_LOGD(TAG, "debug_trigger_timeout: ignored in phase %s",
                 transport_ble_phase_name(phase));
        return;
    }
    ESP_LOGD(TAG, "DEBUG: Manually triggering timeout recovery");
    transport_ble_post_event(PAIRING_EVENT_PHASE_TIMEOUT, NULL, 0U);
}

#endif

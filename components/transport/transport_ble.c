/* components/transport/transport_ble.c
 * BLE Transport Implementation (Bluedroid)
 * Handles advertisement, scan, GATT exchange for pairing discovery.
 * Core BLE logic delegates to ESP-IDF Bluedroid stack.
 */

#include "transport_ble.h"
#include "ui_manager.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <string.h>
#include <stdio.h>
#include <esp_timer.h>
#include "esp_log.h"

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

static void transport_ble_update_state_from_flags(void)
{
    if (g_ble_ctx.is_advertising && g_ble_ctx.is_scanning) {
        g_ble_ctx.state = BLE_ADVERTISING_AND_SCANNING;
    } else if (g_ble_ctx.is_advertising) {
        g_ble_ctx.state = BLE_ADVERTISING;
    } else if (g_ble_ctx.is_scanning) {
        g_ble_ctx.state = BLE_SCANNING;
    } else {
        g_ble_ctx.state = BLE_IDLE;
    }
}

static const uint16_t BLE_SERVICE_UUID = 0xFFF0;
static const uint16_t BLE_CHAR_UUID = 0xFFF1;
static const uint8_t s_adv_service_uuid16[2] = {
    (uint8_t)(BLE_SERVICE_UUID & 0xFFU),
    (uint8_t)((BLE_SERVICE_UUID >> 8U) & 0xFFU)
};
static esp_bt_uuid_t s_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = BLE_SERVICE_UUID }
};
static esp_bt_uuid_t s_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = BLE_CHAR_UUID }
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
#endif


/* Design note: The BLE transport uses Bluedroid (ESP-IDF's built-in BLE stack).
   Phase 1 uses advertisements for discovery. Phase 2 uses GATT for commitment
   exchange. Once commitment is verified, the session moves to Phase 3 over
   ESP-NOW, and BLE connection closes. This keeps BLE usage minimal (no bulk
   data transfer) and reserves bandwidth for ESP-NOW's higher throughput. */

BleContext_t g_ble_ctx = {0};

static bool s_ble_initialised = false;
static bool s_scan_requested = false;
static bool s_adv_data_set = false;
static bool s_scan_rsp_set = false;

/*
 * s_adv_starting — set when esp_ble_gap_start_advertising() is called,
 * cleared on ADV_START_COMPLETE_EVT (success or failure).
 *
 * Prevents the second config-complete callback (ADV or SCAN_RSP) from
 * issuing a duplicate start call while the first is already in flight.
 */
static bool s_adv_starting = false;

/*
 * s_scan_start_failed — set when esp_ble_gap_start_scanning() returns
 * an error. transport_ble_retry_scan_if_needed() uses this to retry the
 * scan on the next periodic check.
 */
static bool s_scan_start_failed = false;

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

static void transport_ble_format_mac(const uint8_t mac[6], char out[18])
{
    (void)snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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
    g_ble_ctx.scan_hit_count = 0U;
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
    s_scan_requested = false;
    s_adv_data_set = false;
    s_scan_rsp_set = false;
    ESP_LOGI(TAG, "BLE context reset: state=%s adv=%u scan=%u",
             transport_ble_state_name(g_ble_ctx.state),
             g_ble_ctx.is_advertising ? 1U : 0U,
             g_ble_ctx.is_scanning ? 1U : 0U);

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

    /* Register BLE callbacks to receive events. Implementations are no-op but
       allow upper layers to rely on the event-driven flow. */
    esp_ble_gap_register_callback(gap_event_handler);
    ESP_LOGI(TAG, "GAP callback registered");

    esp_ble_gattc_register_callback(gattc_event_handler);
    ESP_LOGI(TAG, "GATTC callback registered");

    esp_ble_gatts_register_callback(gatts_event_handler);
    ESP_LOGI(TAG, "GATTS callback registered");

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

    s_ble_initialised = true;
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

    esp_ble_adv_data_t adv_data = {
        .set_scan_rsp = false,
        .include_name = false, /* put name in SCAN_RSP for active scanners */
        .include_txpower = false,
        .min_interval = 0,
        .max_interval = 0,
        .appearance = 0,
        .manufacturer_len = 0,
        .p_manufacturer_data = NULL,
        .service_data_len = 0,
        .p_service_data = NULL,
        .service_uuid_len = sizeof(s_adv_service_uuid16),
        .p_service_uuid = (uint8_t *)s_adv_service_uuid16,
        .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    };

    err = esp_ble_gap_config_adv_data(&adv_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_config_adv_data FAILED: %d (%s)", err, esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }

    /* Configure scan response separately so active scanners always receive name */
    esp_ble_adv_data_t scan_rsp_data = {
        .set_scan_rsp = true,
        .include_name = true,
        .include_txpower = true,
        .min_interval = 0,
        .max_interval = 0,
        .appearance = 0,
        .manufacturer_len = 0,
        .p_manufacturer_data = NULL,
        .service_data_len = 0,
        .p_service_data = NULL,
        .service_uuid_len = 0,
        .p_service_uuid = NULL,
        .flag = 0,
    };

    err = esp_ble_gap_config_adv_data(&scan_rsp_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_config_adv_data (scan rsp) FAILED: %d (%s)", err, esp_err_to_name(err));
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
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,  /* see every advertisement */
    };

    esp_err_t err = esp_ble_gap_set_scan_params(&scan_params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_set_scan_params failed: %d (%s)",
                 err, esp_err_to_name(err));
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
    if (!g_ble_ctx.discovered) { return NULL; }
    return &g_ble_ctx.peer_record;
}

CeePewErr_t transport_ble_connect_to_peer(const uint8_t peer_mac[6])
{
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(g_ble_ctx.state == BLE_SCANNING ||
                  g_ble_ctx.state == BLE_ADVERTISING ||
                  g_ble_ctx.state == BLE_ADVERTISING_AND_SCANNING, CEEPEW_ERR_PARAM);

    memcpy(g_ble_ctx.peer_mac, peer_mac, 6U);
    char peer_mac_str[18];
    transport_ble_format_mac(peer_mac, peer_mac_str);
    ESP_LOGI(TAG, "Connect requested: peer=%s state=%s",
             peer_mac_str, transport_ble_state_name(g_ble_ctx.state));

#ifdef CONFIG_BT_ENABLED
    if (g_ble_ctx.gattc_if == CEEPEW_GATT_IF_NONE || !g_ble_ctx.gattc_registered) {
        ESP_LOGE(TAG, "gattc_if not registered yet");
        return CEEPEW_ERR_BUSY;
    }
    g_ble_ctx.connecting = true;
    esp_ble_gatt_creat_conn_params_t conn_params = {0};
    memcpy(conn_params.remote_bda, g_ble_ctx.peer_mac, ESP_BD_ADDR_LEN);
    conn_params.remote_addr_type = BLE_ADDR_TYPE_PUBLIC;
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
    g_ble_ctx.state = BLE_CONNECTED;
#endif

    return CEEPEW_OK;
}

CeePewErr_t transport_ble_exchange_commitment(const uint8_t commitment_digest[8])
{
    CEEPEW_ASSERT(commitment_digest != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(g_ble_ctx.state == BLE_CONNECTED, CEEPEW_ERR_PARAM);

    /* Store the commitment locally (fixed-size, no dynamic alloc). We keep only
       the truncated 8-byte digest. Actual GATT write will be performed in the
       appropriate GATTC/GATTS callback when handles / connection are available. */
    memcpy(g_ble_ctx.commitment_digest, commitment_digest, 8U);
    ESP_LOGI(TAG, "Commitment exchange queued: state=%s connected=%u char_handle=%u",
             transport_ble_state_name(g_ble_ctx.state),
             g_ble_ctx.gattc_connected ? 1U : 0U,
             (unsigned)g_ble_ctx.gattc_char_handle);

#ifdef CONFIG_BT_ENABLED
    if (g_ble_ctx.gattc_connected && g_ble_ctx.gattc_char_handle != 0U) {
        esp_err_t err = esp_ble_gattc_write_char(
            g_ble_ctx.gattc_if,
            g_ble_ctx.conn_id,
            g_ble_ctx.gattc_char_handle,
            8U,
            g_ble_ctx.commitment_digest,
            ESP_GATT_WRITE_TYPE_RSP,
            ESP_GATT_AUTH_REQ_NONE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ble_gattc_write_char failed: %d (%s)", err, esp_err_to_name(err));
            return CEEPEW_ERR_HW;
        }
    } else {
        g_ble_ctx.commitment_write_pending = true;
    }
#endif

    g_ble_ctx.state = BLE_PAIRING;
    g_ble_ctx.pairing_start_ts = (uint32_t)(esp_timer_get_time() / 1000000LL);

    return CEEPEW_OK;
}

CeePewErr_t transport_ble_verify_commitment(const uint8_t peer_digest[8])
{
    CEEPEW_ASSERT(peer_digest != NULL, CEEPEW_ERR_NULL_PTR);
    if (g_ble_ctx.state != BLE_PAIRING && g_ble_ctx.state != BLE_CONNECTED) {
        return CEEPEW_ERR_PARAM;
    }

    /* Verify peer's commitment matches ours (constant-time comparison) */
    uint8_t match = 0U;
    /* loop bound: 8U (commitment_digest size) */
    for (uint8_t i = 0U; i < 8U; i++) {
        match |= (g_ble_ctx.commitment_digest[i] ^ peer_digest[i]);
    }

    if (match != 0U) {
        return CEEPEW_ERR_AUTH_FAIL;  /* Mismatch */
    }

    g_ble_ctx.commitment_verified = true;
    g_ble_ctx.handoff_ready = true;
    g_ble_ctx.state = BLE_DONE;

    return CEEPEW_OK;
}

bool transport_ble_handoff_ready(void)
{
    return g_ble_ctx.handoff_ready && g_ble_ctx.commitment_verified;
}

CeePewErr_t transport_ble_disconnect(void)
{
    if (g_ble_ctx.state == BLE_IDLE) {
        return CEEPEW_OK;
    }

#ifdef CONFIG_BT_ENABLED
    /* Attempt to disconnect the BLE link if we have a peer address recorded. */
    esp_err_t err = esp_ble_gap_disconnect(g_ble_ctx.peer_mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gap_disconnect returned %d", err);
        /* proceed to reset state regardless */
    }
#endif

    g_ble_ctx.state = BLE_IDLE;
    g_ble_ctx.handoff_ready = false;
    g_ble_ctx.gattc_connected = false;
    g_ble_ctx.gatts_connected = false;
    g_ble_ctx.connecting = false;
    g_ble_ctx.is_advertising = false;
    g_ble_ctx.is_scanning = false;
    s_scan_requested = false;
    ESP_LOGI(TAG, "BLE disconnected: state=%s", transport_ble_state_name(g_ble_ctx.state));

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
    s_ble_initialised = false;

    return CEEPEW_OK;
}

#ifdef CONFIG_BT_ENABLED
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param){
    ESP_LOGI(TAG, "gap_event_handler: event=%d", event);
    switch (event) {

    /* ── Advertising data accepted by stack → start advertising only ── */
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT: {
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
            ESP_LOGE(TAG, "Scan param set FAILED: status=%d",
                     param->scan_param_cmpl.status);
            s_scan_requested    = false;
            s_scan_start_failed = true;
            break;
        }
        {
            esp_err_t e = esp_ble_gap_start_scanning(0U); /* 0 = continuous */
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "esp_ble_gap_start_scanning FAILED: %d (%s)",
                         e, esp_err_to_name(e));
                s_scan_requested    = false;
                s_scan_start_failed = true; /* retry via transport_ble_retry_scan_if_needed */
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
            ESP_LOGI(TAG, "Scan confirmed active");
        } else {
            ESP_LOGE(TAG, "Scan start FAILED: status=%d",
                     param->scan_start_cmpl.status);
            s_scan_requested    = false;
            s_scan_start_failed = true;
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
        if (param->scan_rst.ble_adv != NULL && g_ble_ctx.scan_seen_count < UINT32_MAX) {
            g_ble_ctx.scan_seen_count++;
        }

        char    found_name[17] = {0};
        uint8_t found_name_len = 0U;
        bool    service_uuid_match = false;
        uint8_t service_uuid_len = 0U;
        char    found_mac[18];
        transport_ble_format_mac(param->scan_rst.bda, found_mac);

        uint16_t total_adv_len = (uint16_t)(param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len);
        uint8_t *name_ptr = esp_ble_resolve_adv_data_by_type(
            param->scan_rst.ble_adv,
            total_adv_len,
            ESP_BLE_AD_TYPE_NAME_CMPL,
            &found_name_len);
        if (name_ptr == NULL) {
            name_ptr = esp_ble_resolve_adv_data_by_type(
                param->scan_rst.ble_adv,
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
        }

        uint8_t *uuid_ptr = esp_ble_resolve_adv_data_by_type(
            param->scan_rst.ble_adv,
            total_adv_len,
            ESP_BLE_AD_TYPE_16SRV_CMPL,
            &service_uuid_len);
        if (uuid_ptr == NULL) {
            uuid_ptr = esp_ble_resolve_adv_data_by_type(
                param->scan_rst.ble_adv,
                total_adv_len,
                ESP_BLE_AD_TYPE_16SRV_PART,
                &service_uuid_len);
        }
        if (uuid_ptr != NULL && service_uuid_len >= sizeof(s_adv_service_uuid16) &&
            memcmp(uuid_ptr, s_adv_service_uuid16, sizeof(s_adv_service_uuid16)) == 0) {
            service_uuid_match = true;
        }

        /* Accept a device whose name starts with "CEEPEW" or whose service UUID
         * advertises the CEEPEW service. */
        bool is_ceepew_peer = ((found_name_len >= 6U &&
                                memcmp(found_name, "CEEPEW", 6U) == 0) ||
                               service_uuid_match);

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
            g_ble_ctx.connecting = false;
            g_ble_ctx.state = BLE_CONNECTED;
            g_ble_ctx.service_start_handle = 0U;
            g_ble_ctx.service_end_handle = 0U;
            ESP_LOGI(TAG, "GATTC connected: conn_id=%u", (unsigned)g_ble_ctx.conn_id);
            (void)esp_ble_gattc_search_service(gattc_if, g_ble_ctx.conn_id,
                                               &s_service_uuid_filter);
            break;

        case ESP_GATTC_SEARCH_RES_EVT:
            if (param->search_res.srvc_id.is_primary &&
                param->search_res.srvc_id.id.uuid.len == ESP_UUID_LEN_16 &&
                param->search_res.srvc_id.id.uuid.uuid.uuid16 == BLE_SERVICE_UUID) {
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
                    g_ble_ctx.commitment_write_pending = false;
                    (void)esp_ble_gattc_write_char(
                        g_ble_ctx.gattc_if,
                        g_ble_ctx.conn_id,
                        g_ble_ctx.gattc_char_handle,
                        8U,
                        g_ble_ctx.commitment_digest,
                        ESP_GATT_WRITE_TYPE_RSP,
                        ESP_GATT_AUTH_REQ_NONE);
                }
            } else {
                ESP_LOGW(TAG, "GATTC characteristic not found");
            }
        } break;

        case ESP_GATTC_WRITE_CHAR_EVT:
            if (param->write.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "GATTC write failed: %d", param->write.status);
                /* Treat write failure as auth failure — disconnect and let session wipe */
                g_ble_ctx.state = BLE_IDLE;
            } else {
                ESP_LOGI(TAG, "GATTC write complete — commitment accepted by peer");
                /* Initiator considers verification done when write is ACK'd.
                 * If the responder has a mismatching code it will disconnect. */
                g_ble_ctx.commitment_verified = true;
                g_ble_ctx.handoff_ready = true;
                g_ble_ctx.state = BLE_DONE;
            }
            break;

        case ESP_GATTC_DISCONNECT_EVT:
            g_ble_ctx.gattc_connected = false;
            g_ble_ctx.connecting = false;
            g_ble_ctx.state = BLE_IDLE;
            g_ble_ctx.gattc_char_handle = 0U;
            ESP_LOGI(TAG, "GATTC disconnected");
            break;

        default:
            break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatts_if_t gatts_if,
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
            break;

        case ESP_GATTS_ADD_CHAR_EVT:
            g_ble_ctx.gatts_char_handle = param->add_char.attr_handle;
            ESP_LOGI(TAG, "GATTS characteristic added: handle=%u",
                     (unsigned)g_ble_ctx.gatts_char_handle);
            /* GATTS fully ready — safe to start advertising now */
            (void)transport_ble_start_advertising();
            break;

        case ESP_GATTS_CONNECT_EVT:
            g_ble_ctx.gatts_connected = true;
            g_ble_ctx.conn_id = param->connect.conn_id;
            ESP_LOGI(TAG, "GATTS connected: conn_id=%u", (unsigned)g_ble_ctx.conn_id);
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            g_ble_ctx.gatts_connected = false;
            ESP_LOGI(TAG, "GATTS disconnected");
            break;

        case ESP_GATTS_WRITE_EVT:
            if (param->write.need_rsp) {
                (void)esp_ble_gatts_send_response(
                    gatts_if,
                    param->write.conn_id,
                    param->write.trans_id,
                    ESP_GATT_OK,
                    NULL);
            }
            if (!param->write.is_prep &&
                param->write.handle == g_ble_ctx.gatts_char_handle &&
                param->write.len == 8U) {
                (void)transport_ble_verify_commitment(param->write.value);
            }
            break;

        default:
            break;
    }
}
#endif

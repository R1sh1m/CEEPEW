/* components/transport/transport_ble.c
 *
 * BLE Transport Implementation (Bluedroid)
 * Handles advertisement, scan, GATT exchange for pairing discovery.
 * Core BLE logic delegates to ESP-IDF Bluedroid stack.
 */

#include "transport_ble.h"
#include "ui_manager.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <string.h>
#include <esp_timer.h>

#ifdef CONFIG_BT_ENABLED
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#include "esp_log.h"
#include "esp_err.h"
#endif

static const char *TAG = "transport_ble";
static BlePeerRecord_t s_discovered_peer = {0};

#ifdef CONFIG_BT_ENABLED
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gattc_cb_param_t *param);
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatts_cb_param_t *param);
#endif


/* Design note: The BLE transport uses Bluedroid (ESP-IDF's built-in BLE stack).
   Phase 1 uses advertisements for discovery. Phase 2 uses GATT for commitment
   exchange. Once commitment is verified, the session moves to Phase 3 over
   ESP-NOW, and BLE connection closes. This keeps BLE usage minimal (no bulk
   data transfer) and reserves bandwidth for ESP-NOW's higher throughput. */

BleContext_t g_ble_ctx = {0};

static bool s_ble_initialised = false;

CeePewErr_t transport_ble_init(void)
{
    CEEPEW_ASSERT(!s_ble_initialised, CEEPEW_ERR_BUSY);

    memset(&g_ble_ctx, 0U, sizeof(BleContext_t));
    /* Clear any transient peer info */
    memset(g_ble_ctx.peer_name, 0U, sizeof(g_ble_ctx.peer_name));
    g_ble_ctx.peer_name_len = 0U;
    g_ble_ctx.peer_rssi = 0;
    
    g_ble_ctx.state = BLE_IDLE;
    g_ble_ctx.discovery_start_ts = 0U;
    g_ble_ctx.pairing_start_ts = 0U;
    g_ble_ctx.commitment_verified = false;
    g_ble_ctx.handoff_ready = false;

#ifdef CONFIG_BT_ENABLED
    esp_err_t err;
    /* Initialize BT controller, then bluedroid stack. Some platforms may have
       already initialized the controller; ignore invalid-state returns. */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_bt_controller_init returned %d", err);
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_bt_controller_enable returned %d", err);
    }

    /* Initialize bluedroid if available; some platforms already do controller init elsewhere */
    err = esp_bluedroid_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_bluedroid_init returned %d", err);
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_bluedroid_enable returned %d", err);
    }

    /* Register BLE callbacks to receive events. Implementations are no-op but
       allow upper layers to rely on the event-driven flow. */
    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gattc_register_callback(gattc_event_handler);
    esp_ble_gatts_register_callback(gatts_event_handler);
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
        g_ble_ctx.state != BLE_SCANNING) {
        return CEEPEW_ERR_BUSY;
    }

#ifdef CONFIG_BT_ENABLED
    esp_err_t err;
    /* Set a device name for easier identification during scanning */
    err = esp_ble_gap_set_device_name("CEEPEW");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gap_set_device_name returned %d", err);
    }
    esp_ble_adv_data_t adv_data = {
        .set_scan_rsp = false,
        .include_name = true,
        .include_txpower = false,
        .min_interval = 0,
        .max_interval = 0,
        .appearance = 0,
        .manufacturer_len = 0,
        .p_manufacturer_data = NULL,
        .service_data_len = 0,
        .p_service_data = NULL,
        .service_uuid_len = 0,
        .p_service_uuid = NULL,
        .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    };

    err = esp_ble_gap_config_adv_data(&adv_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_config_adv_data failed: %d", err);
        return CEEPEW_ERR_HW;
    }

    esp_ble_adv_params_t adv_params = {
        .adv_int_min = 0x20,
        .adv_int_max = 0x40,
        .adv_type = ADV_TYPE_IND,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };

    err = esp_ble_gap_start_advertising(&adv_params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_start_advertising failed: %d", err);
        return CEEPEW_ERR_HW;
    }

    /* Also start scanning so we find peers simultaneously (best-effort). */
    err = esp_ble_gap_start_scanning(0); /* continuous */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gap_start_scanning failed: %d", err);
    } else {
        ESP_LOGI(TAG, "BLE scanning started (simultaneous)");
    }
#else
    /* Bluetooth not enabled in sdkconfig; we still update state for higher-level logic */
    (void)0;
#endif

    g_ble_ctx.state = BLE_ADVERTISING;
    g_ble_ctx.discovery_start_ts = (uint32_t)(esp_timer_get_time() / 1000000LL);
    
    return CEEPEW_OK;
}

CeePewErr_t transport_ble_start_scan(void)
{
    CEEPEW_ASSERT(s_ble_initialised, CEEPEW_ERR_PARAM);
    /* Allow starting scan while advertising (ESP32 supports simultaneous roles) */
    if (g_ble_ctx.state != BLE_IDLE &&
        g_ble_ctx.state != BLE_ADVERTISING &&
        g_ble_ctx.state != BLE_SCANNING) {
        return CEEPEW_ERR_BUSY;
    }

#ifdef CONFIG_BT_ENABLED
    esp_err_t err = esp_ble_gap_start_scanning(0); /* continuous */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_start_scanning failed: %d", err);
        return CEEPEW_ERR_HW;
    }
#else
    /* No-op if BT disabled */
    (void)0;
#endif

    g_ble_ctx.state = BLE_SCANNING;
    g_ble_ctx.discovery_start_ts = (uint32_t)(esp_timer_get_time() / 1000000LL);
    
    return CEEPEW_OK;
}

BleState_t transport_ble_get_state(void)
{
    return g_ble_ctx.state;
}

const BlePeerRecord_t *transport_ble_get_peer(void)
{
    /* Populate a transient peer record from g_ble_ctx if discovered. */
    static BlePeerRecord_t tmp = {0};
    if (!g_ble_ctx.discovered) { return NULL; }
    memcpy(tmp.peer_mac, g_ble_ctx.peer_mac, 6U);
    tmp.rssi = g_ble_ctx.peer_rssi;
    tmp.seen_at = g_ble_ctx.discovery_start_ts;
    tmp.name_len = g_ble_ctx.peer_name_len;
    if (tmp.name_len > 15U) { tmp.name_len = 15U; }
    if (tmp.name_len > 0U) {
        memcpy(tmp.name, g_ble_ctx.peer_name, tmp.name_len);
    }
    tmp.name[tmp.name_len] = '\0';
    return &tmp;
}

CeePewErr_t transport_ble_connect_to_peer(const uint8_t peer_mac[6])
{
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(g_ble_ctx.state == BLE_SCANNING ||
                  g_ble_ctx.state == BLE_ADVERTISING, CEEPEW_ERR_PARAM);

    memcpy(g_ble_ctx.peer_mac, peer_mac, 6U);

#ifdef CONFIG_BT_ENABLED
    esp_err_t err = esp_ble_gattc_open(ESP_GATT_IF_NONE, g_ble_ctx.peer_mac, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gattc_open failed: %d", err);
        return CEEPEW_ERR_HW;
    }
    /* Actual state transition will occur in GATTC callback when connected */
#else
    /* If BT disabled, simulate immediate connection for higher-level logic */
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

#ifdef CONFIG_BT_ENABLED
    /* If desired, trigger an application-level GATT write here once conn/handles known. */
    (void)0;
#endif

    g_ble_ctx.state = BLE_PAIRING;
    g_ble_ctx.pairing_start_ts = (uint32_t)(esp_timer_get_time() / 1000000LL);
    
    return CEEPEW_OK;
}

CeePewErr_t transport_ble_verify_commitment(const uint8_t peer_digest[8])
{
    CEEPEW_ASSERT(peer_digest != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(g_ble_ctx.state == BLE_PAIRING, CEEPEW_ERR_PARAM);

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
    CEEPEW_ASSERT(g_ble_ctx.state == BLE_DONE, CEEPEW_ERR_PARAM);

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
    s_ble_initialised = false;
    
    return CEEPEW_OK;
}

#ifdef CONFIG_BT_ENABLED
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {

        /* Advertising data configured: now start advertising and scanning */
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT: {
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
                ESP_LOGW(TAG, "start_advertising failed: %d", e);
            } else {
                ESP_LOGI(TAG, "BLE advertising started");
                g_ble_ctx.state = BLE_ADVERTISING;
            }

            /* Also start scanning so we find peers simultaneously */
            e = esp_ble_gap_start_scanning(0);   /* 0 = continuous */
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "start_scanning failed: %d", e);
            } else {
                ESP_LOGI(TAG, "BLE scanning started (simultaneous)");
            }
        } break;

        /* Scan result: only accept other CEEPEW devices */
        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            if (!param) { break; }
            if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) { break; }

            /* Extract device name from AD structures */
            const uint8_t *adv    = param->scan_rst.adv_data;
            uint8_t        adv_len = param->scan_rst.adv_data_len;
            char           found_name[17] = {0};
            uint8_t        found_name_len = 0U;

            uint8_t idx = 0U;
            while (idx + 1U < adv_len) {
                uint8_t field_len  = adv[idx];
                if (field_len == 0U) { break; }
                if ((uint16_t)idx + 1U + field_len > adv_len) { break; }
                uint8_t field_type = adv[idx + 1U];

                if (field_type == 0x09U || field_type == 0x08U) {   /* complete/shortened name */
                    found_name_len = (uint8_t)(field_len - 1U);
                    if (found_name_len > 16U) { found_name_len = 16U; }
                    memcpy(found_name, &adv[idx + 2U], found_name_len);
                    found_name[found_name_len] = '\0';
                    break;
                }
                idx = (uint8_t)(idx + field_len + 1U);
            }

            /* Only accept peers whose name begins with "CEEPEW" */
            bool is_ceepew_peer = (found_name_len >= 6U &&
                                   memcmp(found_name, "CEEPEW", 6U) == 0);

            if (!is_ceepew_peer) {
                /* Ignore non-CEEPEW devices */
                break;
            }

            /* Avoid re-processing the same peer we already found */
            if (g_ble_ctx.discovered &&
                memcmp(g_ble_ctx.peer_mac, param->scan_rst.bda, 6U) == 0) {
                break;  /* already logged this peer */
            }

            ESP_LOGI(TAG, "CEEPEW peer found: %02X:%02X:%02X:%02X:%02X:%02X  RSSI=%d  name=%s",
                     param->scan_rst.bda[0], param->scan_rst.bda[1],
                     param->scan_rst.bda[2], param->scan_rst.bda[3],
                     param->scan_rst.bda[4], param->scan_rst.bda[5],
                     param->scan_rst.rssi,
                     found_name);

            /* Record the peer */
            memcpy(s_discovered_peer.peer_mac, param->scan_rst.bda, 6U);
            s_discovered_peer.rssi    = (int8_t)param->scan_rst.rssi;
            s_discovered_peer.seen_at = (uint32_t)(esp_timer_get_time() / 1000000LL);
            s_discovered_peer.name_len = found_name_len;
            memcpy(s_discovered_peer.name, found_name, found_name_len);
            s_discovered_peer.name[found_name_len] = '\0';

            memcpy(g_ble_ctx.peer_mac,   param->scan_rst.bda,  6U);
            memcpy(g_ble_ctx.peer_name,  found_name,          found_name_len);
            g_ble_ctx.peer_name_len  = found_name_len;
            g_ble_ctx.peer_rssi      = (int8_t)param->scan_rst.rssi;
            g_ble_ctx.discovered     = true;
            g_ble_ctx.discovery_start_ts = (uint32_t)(esp_timer_get_time() / 1000000LL);

            (void)ui_manager_transition_to(UI_STATE_DISCOVERY);
        } break;

        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            ESP_LOGI(TAG, "Scan start complete, status=%d",
                     param ? param->scan_start_cmpl.status : -1);
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            ESP_LOGI(TAG, "Adv start complete, status=%d",
                     param ? param->adv_start_cmpl.status : -1);
            break;

        default:
            break;
    }
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gattc_cb_param_t *param)
{
    (void)event; (void)param;
    /* For this implementation we only scaffold callbacks. Real app should
       update connection state, obtain handles, and perform writes/reads. */
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatts_cb_param_t *param)
{
    (void)event; (void)param;
    /* Server-side callbacks would accept writes from peer and call
       transport_ble_verify_commitment() with received 8-byte digest. */
}
#endif

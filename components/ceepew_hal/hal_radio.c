/* components/ceepew_hal/hal_radio.c
 *
 * ESP-NOW radio HAL. Owns WiFi init, ESP-NOW init, peer registration.
 * Must be initialised ONCE at session start, not per-packet.
 *
 * Security note: hal_radio is below the ESL (ESP-NOW Security Layer).
 * It provides raw delivery only — authentication, encryption, replay
 * protection are applied by transport_esl before data reaches here.
 *
 * RX Queue:
 *   - Created in hal_radio_init() with capacity CEEPEW_QUEUE_DEPTH (8 frames)
 *   - Populated by radio_recv_cb() ISR via xQueueSendFromISR()
 *   - If queue full, newest frame is dropped silently
 *   - Accessed by session task via hal_radio_get_rx_queue()
 */

#include "hal_radio.h"
#include "hal_pins.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "../transport/transport_hop.h"
#include "../crypto/crypto_ctx.h"

#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "nvs_flash.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "hal_radio";

/* Module state — zero-initialised at file scope */
static bool    s_initialised    = false;
static bool    s_peer_set       = false;
static uint8_t s_peer_mac[6]    = {0};
static bool    s_peer_added     = false;

/* Spinlock protecting peer state (s_peer_mac, s_peer_set, s_peer_added)
 * Used by ISR (radio_recv_cb) and task contexts (hal_radio_set_peer, hal_radio_send, hal_radio_is_peer_registered) */
static portMUX_TYPE s_peer_mux = portMUX_INITIALIZER_UNLOCKED;

/* RX Queue — created in hal_radio_init(), accessed via hal_radio_get_rx_queue() */
static QueueHandle_t s_rx_queue = NULL;

/* RX queue overrun counter — incremented when xQueueOverwriteFromISR overwrites oldest frame.
 * Used for diagnostics to detect sustained burst traffic exceeding queue capacity. */
static uint32_t s_rx_queue_overruns = 0U;

/* Channel hopping task — created in hal_radio_init(), started after Phase 3 */
static TaskHandle_t  s_hop_task_handle = NULL;
static esp_timer_handle_t s_hop_timer = NULL;

/* Crypto context pointer for channel hopping callback — set by caller */
static const CryptoCtx_t *s_hop_crypto_ctx = NULL;

/* Nonce counter getter callback — set by caller */
static hal_radio_get_nonce_counter_cb_t s_nonce_getter = NULL;

/* Hop task control */
static volatile bool s_hop_task_running = false;

/* ── Internal helpers ───────────────────────────────────────────── */

/* Constant-time MAC comparison — avoids early-exit timing leak */
static bool mac_equal_ct(const uint8_t a[6], const uint8_t b[6]){
    uint8_t diff = 0U;
    /* loop bound: 6 (compile-time constant) */
    for (uint8_t i = 0U; i < 6U; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);}
    return (diff == 0U);
}

static bool mac_is_zero(const uint8_t mac[6]){
    static const uint8_t zero[6] = {0};
    return mac_equal_ct(mac, zero);
}

/* ── ESP-NOW callbacks ─────────────────────────────────────────── */

/* ESP-NOW send callback (ESP-IDF v6 signature):
 *   void (*esp_now_send_cb_t)(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
 */
/* Send status callback registered by upper layers (optional) */
static hal_radio_send_status_cb_t s_send_status_cb = NULL;

static void radio_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status){
    CEEPEW_ASSERT_VOID(tx_info != NULL);
    (void)tx_info;
    
    const char *status_str = (status == ESP_NOW_SEND_SUCCESS) ? "OK" : "FAIL";
    
    #ifdef CEEPEW_DEBUG_SERIAL
    ESP_LOGI(TAG, "espnow_send_cb: status=%s peer=%02X:%02X:%02X:%02X:%02X:%02X",
        status_str,
        tx_info->des_addr[0], tx_info->des_addr[1], tx_info->des_addr[2],
        tx_info->des_addr[3], tx_info->des_addr[4], tx_info->des_addr[5]);
    #endif
    
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(TAG, "ESP-NOW TX OK -> %02X:%02X:%02X:%02X:%02X:%02X",
                 tx_info->des_addr[0], tx_info->des_addr[1], tx_info->des_addr[2],
                 tx_info->des_addr[3], tx_info->des_addr[4], tx_info->des_addr[5]);
    } else {
        ESP_LOGW(TAG, "ESP-NOW TX FAILED (status=%d, %s) -> %02X:%02X:%02X:%02X:%02X:%02X",
                 (int)status, status_str,
                 tx_info->des_addr[0], tx_info->des_addr[1], tx_info->des_addr[2],
                 tx_info->des_addr[3], tx_info->des_addr[4], tx_info->des_addr[5]);
    }

    /* Notify registered upper-layer callback if present */
    if (s_send_status_cb != NULL) {
        s_send_status_cb(status);
    }
}

/* Check if ESP-NOW peer is currently registered */
bool hal_radio_is_peer_registered(const uint8_t peer_mac[6]) {
    if (!s_initialised || peer_mac == NULL) {
        return false;
    }
    uint8_t stored_mac[6];
    bool peer_set;
    portENTER_CRITICAL(&s_peer_mux);
    peer_set = s_peer_set;
    memcpy(stored_mac, s_peer_mac, 6U);
    portEXIT_CRITICAL(&s_peer_mux);
    if (!peer_set) {
        return false;
    }
    return memcmp(stored_mac, peer_mac, 6U) == 0;
}

CeePewErr_t hal_radio_set_send_status_cb(hal_radio_send_status_cb_t cb){
    /* Allow registration before full radio init; the callback will be invoked
     * if/when the ESP-NOW send callback is registered during hal_radio_init().
     */
    s_send_status_cb = cb;
    return CEEPEW_OK;
}

/* Receive callback — invoked by WiFi task on Core 0.
 * ESP-IDF v6 recv signature:
 *   void (*esp_now_recv_cb_t)(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len)
 *
 * Order of operations (security-critical):
 *   1. Validate frame structure (CEEPEW_ASSERT_VOID)
 *   2. Check MAC lock — silently discard if peer mismatch
 *   3. Validate payload length
 *   4. Construct RadioFrame_t
 *   5. Post to RX queue via xQueueSendFromISR()
 *   6. No error response — ISR must complete < 1ms
 */
static void radio_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len){
    CEEPEW_ASSERT_VOID(esp_now_info != NULL);
    CEEPEW_ASSERT_VOID(esp_now_info->src_addr != NULL && data != NULL);
    CEEPEW_ASSERT_VOID(data_len > 0 && data_len <= ESP_NOW_MAX_DATA_LEN);

    #ifdef CEEPEW_DEBUG_SERIAL
    int rssi = 0;
    if (esp_now_info->rx_ctrl != NULL) {
        rssi = esp_now_info->rx_ctrl->rssi;
    }
    ESP_LOGI(TAG, "espnow_recv_cb: from=%02X:%02X:%02X:%02X:%02X:%02X len=%d rssi=%d",
        esp_now_info->src_addr[0], esp_now_info->src_addr[1], esp_now_info->src_addr[2],
        esp_now_info->src_addr[3], esp_now_info->src_addr[4], esp_now_info->src_addr[5],
        data_len, rssi);
    #endif

    /* MAC lock: after pairing, silently discard frames from unknown peers */
    uint8_t peer_mac[6];
    portENTER_CRITICAL_ISR(&s_peer_mux);
    bool peer_set = s_peer_set;
    memcpy(peer_mac, s_peer_mac, 6U);
    portEXIT_CRITICAL_ISR(&s_peer_mux);

    if (peer_set && !mac_equal_ct(esp_now_info->src_addr, peer_mac)) {
        ESP_LOGD(TAG, "RX from unexpected MAC — discarded");
        return;
    }

    /* Validate payload length before queue post */
    if ((uint16_t)data_len > ESP_NOW_MAX_DATA_LEN) {
        ESP_LOGD(TAG, "RX frame exceeds max payload");
        return;
    }

    /* Construct frame for queue post */
    RadioFrame_t frame = {0};
    memcpy(frame.src_mac, esp_now_info->src_addr, CEEPEW_DEVICE_ID_BYTES);
    if (esp_now_info->des_addr != NULL) {
        memcpy(frame.dst_mac, esp_now_info->des_addr, CEEPEW_DEVICE_ID_BYTES);
    }
    /* channel and rssi not available in esp_now_recv_info_t for this ESP-IDF version */
    frame.channel      = 0U;
    frame.rssi         = 0;
    frame.timestamp_us = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFULL);
    memcpy(frame.payload, data, (uint32_t)data_len);
    frame.payload_len  = (uint16_t)data_len;

    /* Post to RX queue — ISR-safe, no blocking.
     * Uses xQueueOverwriteFromISR to drop oldest frame when full,
     * preserving newer frames with higher nonce counters. */
    if (s_rx_queue != NULL) {
        BaseType_t was_woken = pdFALSE;
        BaseType_t rc = xQueueOverwriteFromISR(s_rx_queue, &frame, &was_woken);
        if (rc == pdTRUE) {
            /* Queue was full and oldest frame was overwritten.
             * Increment counter and log for diagnostics (rate-limited). */
            s_rx_queue_overruns++;
            if ((s_rx_queue_overruns % 100U) == 0U) {
                ESP_LOGW(TAG, "RX queue overrun count: %lu (oldest frame dropped)",
                         (unsigned long)s_rx_queue_overruns);
            }
        }
        if (was_woken == pdTRUE) {
            portYIELD_FROM_ISR(was_woken);
        }
    }
}

/* Channel hopping task — runs on Core 1, priority 2.
 * Waits for periodic timer notification, computes next channel, applies it.
 */
static void hal_radio_hop_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Channel hopping task started on Core %d", xPortGetCoreID());

    for (;;) {
        /* Wait for timer notification (periodic, CEEPEW_HOP_INTERVAL_MS) */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!s_hop_task_running) {
            break;  /* Stop requested */
        }

        if (!s_hop_crypto_ctx || !s_nonce_getter) {
            continue;
        }

        /* Get next channel from session state */
        uint8_t next_ch = 0U;
        uint64_t nonce_counter = s_nonce_getter();
        CeePewErr_t err = transport_get_current_channel(s_hop_crypto_ctx, nonce_counter, &next_ch);
        if (err != CEEPEW_OK) {
            ESP_LOGD(TAG, "Hop failed: transport_get_current_channel returned %d", (int)err);
            continue;
        }

        /* Set radio to next channel */
        err = hal_radio_set_channel(next_ch);
        if (err != CEEPEW_OK) {
            ESP_LOGD(TAG, "Hop failed: hal_radio_set_channel(%u) returned %d", (unsigned)next_ch, (int)err);
            continue;
        }

        ESP_LOGD(TAG, "Hopped to channel %u", (unsigned)next_ch);
    }

    vTaskDelete(NULL);
}

/* Periodic timer callback — notifies hop task */
static void hop_timer_cb(void *arg)
{
    (void)arg;
    if (s_hop_task_handle != NULL && s_hop_task_running) {
        xTaskNotifyGive(s_hop_task_handle);
    }
}

/* ── WiFi/ESP-NOW init ─────────────────────────────────────────── */

static void radio_cleanup(void){
    (void)esp_now_deinit();
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
    
    /* Clean up queue on radio deinit */
    if (s_rx_queue != NULL) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }
    
    s_initialised = false;
    s_peer_added  = false;
}

static CeePewErr_t radio_wifi_start(void){
    CEEPEW_ASSERT(CEEPEW_ESPNOW_CHANNEL >= 1U && CEEPEW_ESPNOW_CHANNEL <= 13U, CEEPEW_ERR_PARAM);
    esp_err_t rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        rc = nvs_flash_erase();
        if (rc != ESP_OK) { return CEEPEW_ERR_HW; }
        rc = nvs_flash_init();
    }
    if (rc != ESP_OK) { return CEEPEW_ERR_HW; }

    rc = esp_netif_init();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) { return CEEPEW_ERR_HW; }

    rc = esp_event_loop_create_default();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) { return CEEPEW_ERR_HW; }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    rc = esp_wifi_init(&cfg);
    if (rc != ESP_OK) { return CEEPEW_ERR_HW; }

    /* Keep WiFi state in RAM only — no flash persistence */
    rc = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    /* Station mode: required for ESP-NOW channel control */
    rc = esp_wifi_set_mode(WIFI_MODE_STA);
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    /* Disable power saving: reduces ESP-NOW latency significantly */
    rc = esp_wifi_set_ps(WIFI_PS_NONE);
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    rc = esp_wifi_start();
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    /* Fix channel: both devices must hop in sync */
    rc = esp_wifi_set_channel((uint8_t)CEEPEW_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }
    return CEEPEW_OK;
}

/* ── Public API ────────────────────────────────────────────────── */

CeePewErr_t hal_radio_init(void)
{
    CEEPEW_ASSERT(CEEPEW_ESPNOW_CHANNEL >= 1U &&
                  CEEPEW_ESPNOW_CHANNEL <= 13U, CEEPEW_ERR_PARAM);

    /* Idempotent: safe to call multiple times (only initialises once) */
    if (s_initialised) {
        return CEEPEW_OK;
    }

    CeePewErr_t err = radio_wifi_start();
    if (err != CEEPEW_OK) { return err; }

    /* Create RX queue — depth CEEPEW_QUEUE_DEPTH (8), item size sizeof(RadioFrame_t) */
    CEEPEW_ASSERT(CEEPEW_QUEUE_DEPTH > 0U, CEEPEW_ERR_PARAM);
    s_rx_queue = xQueueCreate((uint32_t)CEEPEW_QUEUE_DEPTH, sizeof(RadioFrame_t));
    CEEPEW_ASSERT(s_rx_queue != NULL, CEEPEW_ERR_BOUNDS);

    esp_err_t rc = esp_now_init();
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    rc = esp_now_register_send_cb(radio_send_cb);
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    rc = esp_now_register_recv_cb(radio_recv_cb);
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    /* Create channel hopping task (pinned to Core 1, priority 2) */
    BaseType_t task_rc = xTaskCreatePinnedToCore(
        hal_radio_hop_task,
        "ChannelHop",
        4096,  /* 4KB stack */
        NULL,
        2,     /* priority 2 (below session task) */
        &s_hop_task_handle,
        1      /* Core 1 */
    );
    if (task_rc != pdPASS) {
        radio_cleanup();
        return CEEPEW_ERR_HW;
    }

    /* Create periodic esp_timer for hop task notification */
    const esp_timer_create_args_t timer_args = {
        .callback = hop_timer_cb,
        .arg = NULL,
        .name = "hop_timer"
    };
    esp_err_t timer_rc = esp_timer_create(&timer_args, &s_hop_timer);
    if (timer_rc != ESP_OK) {
        radio_cleanup();
        return CEEPEW_ERR_HW;
    }

    s_initialised = true;
    ESP_LOGI(TAG, "ESP-NOW ready on ch%u, RX queue depth %u",
             (unsigned)CEEPEW_ESPNOW_CHANNEL, (unsigned)CEEPEW_QUEUE_DEPTH);
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_set_recv_cb(esp_now_recv_cb_t cb){
    CEEPEW_ASSERT(cb != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_initialised, CEEPEW_ERR_BUSY);
    /* Note: RX queue is now the primary reception mechanism.
       Set callback only if additional processing is needed. */
    (void)cb;  /* Callback pattern deprecated in favor of queue — reserved for future. */
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_set_peer(const uint8_t peer_mac[6]){
    return hal_radio_set_peer_with_lmk(peer_mac, NULL);
}

CeePewErr_t hal_radio_set_peer_with_lmk(const uint8_t peer_mac[6], const uint8_t lmk[16]){
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(!mac_is_zero(peer_mac), CEEPEW_ERR_PARAM);

    uint8_t old_peer[6] = {0};
    bool peer_was_added = false;
    portENTER_CRITICAL(&s_peer_mux);
    if (s_peer_added) {
        /* Preserve the currently-registered peer before overwriting */
        memcpy(old_peer, s_peer_mac, 6U);
        peer_was_added = true;
    }
    portEXIT_CRITICAL(&s_peer_mux);

    if (!s_initialised) { return CEEPEW_OK;   /* will be registered when init() is called */}

    /* Remove old peer if registered (delete using preserved old_peer) */
    if (peer_was_added) {
        (void)esp_now_del_peer(old_peer);
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, peer_mac, 6U);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = (uint8_t)CEEPEW_ESPNOW_CHANNEL;
    
    if (lmk != NULL) {
        peer.encrypt = true;
        memcpy(peer.lmk, lmk, 16U);
    } else {
        peer.encrypt = false;   /* ESL layer handles encryption above ESP-NOW */
    }

    esp_err_t rc = esp_now_add_peer(&peer);
    CEEPEW_ASSERT(rc == ESP_OK, CEEPEW_ERR_HW);

    /* Only update software peer MAC AFTER hardware peer is successfully added.
     * This prevents race where recv_cb sees new MAC but frame is from old peer. */
    portENTER_CRITICAL(&s_peer_mux);
    memcpy(s_peer_mac, peer_mac, 6U);
    s_peer_set = true;
    s_peer_added = true;
    portEXIT_CRITICAL(&s_peer_mux);
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_send(const uint8_t *buf, uint16_t len){
    CEEPEW_ASSERT(s_initialised,  CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(buf != NULL,    CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= (uint16_t)ESP_NOW_MAX_DATA_LEN, CEEPEW_ERR_BOUNDS);

    uint8_t peer_mac[6];
    bool peer_set;
    portENTER_CRITICAL(&s_peer_mux);
    peer_set = s_peer_set;
    memcpy(peer_mac, s_peer_mac, 6U);
    portEXIT_CRITICAL(&s_peer_mux);

    if (!peer_set) {
        return CEEPEW_ERR_PARAM;
    }

    esp_err_t rc = esp_now_send(peer_mac, buf, (size_t)len);
    switch (rc) {
        case ESP_OK:                    return CEEPEW_OK;
        case ESP_ERR_ESPNOW_NOT_FOUND:  return CEEPEW_ERR_PARAM;
        case ESP_ERR_ESPNOW_IF:         return CEEPEW_ERR_HW;
        default:                        return CEEPEW_ERR_HW;
    }
    /* Note: CEEPEW_OK is returned by the send callback on confirmed delivery.
     * Callers must use ACK-based confirmation (ARQ layer), not this return value. */
}

CeePewErr_t hal_radio_set_channel(uint8_t channel){
    CEEPEW_ASSERT(s_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(channel >= 1U && channel <= 13U, CEEPEW_ERR_PARAM);

    esp_err_t rc = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    CEEPEW_ASSERT(rc == ESP_OK, CEEPEW_ERR_HW);
    return CEEPEW_OK;
}

QueueHandle_t hal_radio_get_rx_queue(void){
    return s_rx_queue;
}

CeePewErr_t hal_radio_set_power_save(wifi_ps_type_t ps_mode){
    CEEPEW_ASSERT(s_initialised, CEEPEW_ERR_BUSY);
    esp_err_t rc = esp_wifi_set_ps(ps_mode);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(%d) failed: %d", (int)ps_mode, (int)rc);
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "WiFi PS mode set to %d", (int)ps_mode);
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_set_hop_context(const void *crypto_ctx, hal_radio_get_nonce_counter_cb_t nonce_getter){
    CEEPEW_ASSERT(crypto_ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(nonce_getter != NULL, CEEPEW_ERR_NULL_PTR);
    
    s_hop_crypto_ctx = (const CryptoCtx_t *)crypto_ctx;
    s_nonce_getter = nonce_getter;
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_start_channel_hopping(void)
{
    CEEPEW_ASSERT(s_initialised, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_hop_timer != NULL, CEEPEW_ERR_HW);
    CEEPEW_ASSERT(s_hop_crypto_ctx != NULL, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_nonce_getter != NULL, CEEPEW_ERR_PARAM);

    esp_err_t rc = esp_timer_start_periodic(s_hop_timer, CEEPEW_HOP_INTERVAL_MS * 1000);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start channel hopping timer: %d", rc);
        return CEEPEW_ERR_HW;
    }
    s_hop_task_running = true;
    ESP_LOGI(TAG, "Channel hopping started (interval=%u ms)", CEEPEW_HOP_INTERVAL_MS);
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_stop_channel_hopping(void)
{
    if (s_hop_timer != NULL) {
        esp_timer_stop(s_hop_timer);
        esp_timer_delete(s_hop_timer);
        s_hop_timer = NULL;
    }
    s_hop_task_running = false;
    if (s_hop_task_handle != NULL) {
        xTaskNotifyGive(s_hop_task_handle);  /* Wake task to exit */
        /* Wait for task to actually delete itself before clearing handle.
         * This prevents use-after-free if timer callback fires during shutdown. */
        vTaskDelay(pdMS_TO_TICKS(100));
        s_hop_task_handle = NULL;
    }
    return CEEPEW_OK;
}
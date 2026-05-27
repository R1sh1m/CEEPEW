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

/* RX Queue — created in hal_radio_init(), accessed via hal_radio_get_rx_queue() */
static QueueHandle_t s_rx_queue = NULL;

/* Channel hopping timer — created in hal_radio_init(), started after Phase 3 */
static TimerHandle_t  s_channel_hop_timer = NULL;
static StaticTimer_t  s_channel_hop_timer_buf;

/* Crypto context pointer for channel hopping callback — set by caller */
static const CryptoCtx_t *s_hop_crypto_ctx = NULL;

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
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(TAG, "ESP-NOW TX OK");
    } else {
        ESP_LOGW(TAG, "ESP-NOW TX FAILED (status=%d)", (int)status);
    }

    /* Notify registered upper-layer callback if present */
    if (s_send_status_cb != NULL) {
        s_send_status_cb(status);
    }
}

CeePewErr_t hal_radio_set_send_status_cb(hal_radio_send_status_cb_t cb){
    CEEPEW_ASSERT(s_initialised, CEEPEW_ERR_BUSY);
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

    /* MAC lock: after pairing, silently discard frames from unknown peers */
    if (s_peer_set && !mac_equal_ct(esp_now_info->src_addr, s_peer_mac)) {
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

    /* Post to RX queue — ISR-safe, no blocking */
    if (s_rx_queue != NULL) {
        BaseType_t was_woken = pdFALSE;
        BaseType_t rc = xQueueSendFromISR(s_rx_queue, &frame, &was_woken);
        if (rc != pdPASS) {
            /* Queue full: drop frame silently */
            ESP_LOGD(TAG, "RX queue full — frame dropped");
        }
        if (was_woken == pdTRUE) {
            portYIELD_FROM_ISR(was_woken);
        }
    }
}

/* Channel hopping timer callback — invoked every CEEPEW_HOP_INTERVAL_MS by FreeRTOS.
 * Gets next channel from current session nonce state, applies it to radio.
 * Callback must complete in < 1ms and make no blocking calls.
 */
static void radio_hop_timer_callback(TimerHandle_t xTimer)
{
    CEEPEW_ASSERT_VOID(xTimer != NULL);
    CEEPEW_ASSERT_VOID(s_hop_crypto_ctx != NULL);

    /* Get next channel from session state */
    uint8_t next_ch = 0U;
    CeePewErr_t err = transport_get_current_channel(s_hop_crypto_ctx, &next_ch);
    if (err != CEEPEW_OK) {
        ESP_LOGD(TAG, "Hop failed: transport_get_current_channel returned %d", (int)err);
        return;
    }

    /* Set radio to next channel — non-blocking */
    err = hal_radio_set_channel(next_ch);
    if (err != CEEPEW_OK) {
        ESP_LOGD(TAG, "Hop failed: hal_radio_set_channel(%u) returned %d", (unsigned)next_ch, (int)err);
        return;
    }

    ESP_LOGD(TAG, "Hopped to channel %u", (unsigned)next_ch);
    (void)xTimer;  /* unused parameter */
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

    /* Create channel hopping timer (static allocation, not started yet) */
    /* Timer will be started by session FSM after Phase 3 (pairing complete) */
    s_channel_hop_timer = xTimerCreateStatic(
        "ChannelHop",                                   /* timer name */
        pdMS_TO_TICKS((uint32_t)CEEPEW_HOP_INTERVAL_MS), /* period in ticks */
        pdTRUE,                                         /* auto-reload */
        (void *)0,                                      /* timer ID (unused) */
        radio_hop_timer_callback,                       /* callback */
        &s_channel_hop_timer_buf                        /* static buffer */
    );
    CEEPEW_ASSERT(s_channel_hop_timer != NULL, CEEPEW_ERR_BOUNDS);

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
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(!mac_is_zero(peer_mac), CEEPEW_ERR_PARAM);

    uint8_t old_peer[6] = {0};
    if (s_peer_added) {
        /* Preserve the currently-registered peer before overwriting */
        memcpy(old_peer, s_peer_mac, 6U);
    }

    memcpy(s_peer_mac, peer_mac, 6U);
    s_peer_set = true;

    if (!s_initialised) { return CEEPEW_OK;   /* will be registered when init() is called */}

    /* Remove old peer if registered (delete using preserved old_peer) */
    if (s_peer_added) {
        (void)esp_now_del_peer(old_peer);
        s_peer_added = false;
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, peer_mac, 6U);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = (uint8_t)CEEPEW_ESPNOW_CHANNEL;
    peer.encrypt = false;   /* ESL layer handles encryption above ESP-NOW */

    esp_err_t rc = esp_now_add_peer(&peer);
    CEEPEW_ASSERT(rc == ESP_OK, CEEPEW_ERR_HW);

    s_peer_added = true;
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_send(const uint8_t *buf, uint16_t len){
    CEEPEW_ASSERT(s_initialised,  CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(s_peer_set,     CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(buf != NULL,    CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= (uint16_t)ESP_NOW_MAX_DATA_LEN, CEEPEW_ERR_BOUNDS);
    esp_err_t rc = esp_now_send(s_peer_mac, buf, (size_t)len);
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
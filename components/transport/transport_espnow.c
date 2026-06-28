/* components/transport/transport_espnow.c */

#include "ceepew_assert.h"
#include "hal_radio.h"
#include "hal_pins.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "transport_esl.h"
#include "session_fsm.h"

static const char *TAG = "transport_espnow";

static SemaphoreHandle_t s_send_sem = NULL;
static esp_now_send_status_t s_last_send_status = ESP_NOW_SEND_FAIL;

static void transport_send_status_cb(esp_now_send_status_t status);
static CeePewErr_t transport_espnow_ensure_sync(void)
{
    if (s_send_sem == NULL) {
        s_send_sem = xSemaphoreCreateBinary();
        if (s_send_sem == NULL) {
            return CEEPEW_ERR_HW;
        }
    }

    (void)hal_radio_set_send_status_cb(transport_send_status_cb);
    return CEEPEW_OK;
}

static void transport_send_status_cb(esp_now_send_status_t status)
{
    s_last_send_status = status;
    
    #ifdef CEEPEW_DEBUG_SERIAL
    ESP_LOGI(TAG, "espnow_send_cb: status=%s", 
        status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
    #endif
    
    if (s_send_sem != NULL) {
        BaseType_t was_woken = pdFALSE;
        (void)xSemaphoreGiveFromISR(s_send_sem, &was_woken);
        if (was_woken == pdTRUE) {
            portYIELD_FROM_ISR(was_woken);
        }
    }
}

CeePewErr_t transport_espnow_init(void){
    CEEPEW_ASSERT(CEEPEW_ESPNOW_CHANNEL >= 1 && CEEPEW_ESPNOW_CHANNEL <= 13, CEEPEW_ERR_PARAM);
    return transport_espnow_ensure_sync();
}

CeePewErr_t transport_espnow_send(const uint8_t *peer_mac, const uint8_t *data, uint16_t len){
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(data != NULL || len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(len <= ESP_NOW_MAX_DATA_LEN, CEEPEW_ERR_BOUNDS);

    CeePewErr_t sync_err = transport_espnow_ensure_sync();
    if (sync_err != CEEPEW_OK) {
        return sync_err;
    }

    CeePewErr_t err = hal_radio_set_peer(peer_mac);
    if (err != CEEPEW_OK){ return err;}
    err = hal_radio_init();
    if (err != CEEPEW_OK) { return err; }

    /* Clear any previous send semaphore state */
    if (s_send_sem != NULL) {
        xSemaphoreTake(s_send_sem, 0U);
    }

    #ifdef CEEPEW_DEBUG_SERIAL
    ESP_LOGI(TAG, "transport_espnow_send: peer=%02X:%02X:%02X:%02X:%02X:%02X len=%d",
        peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5], len);
    #endif

    return hal_radio_send(data, len);
}

/* Wait for send-completion event from hal_radio (send callback). This is not a
 * peer-level ACK; it only confirms the local radio stack completed the send
 * operation. Use this as a pragmatic improvement over a no-op ACK stub.
 */
CeePewErr_t transport_wait_ack(const uint8_t *peer_mac, uint16_t seq, uint32_t timeout_ms){
    (void)peer_mac;
    (void)seq;

    CeePewErr_t sync_err = transport_espnow_ensure_sync();
    if (sync_err != CEEPEW_OK) {
        return sync_err;
    }

    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_send_sem, ticks) == pdTRUE) {
        if (s_last_send_status == ESP_NOW_SEND_SUCCESS) { return CEEPEW_OK; }
        return CEEPEW_ERR_HW;
    }
    return CEEPEW_ERR_TIMEOUT;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Rendezvous Phase (Static Channel Sync before Channel Hopping)               */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Drive rendezvous handshake from session task.
 * Initiator: sends REQ, waits for ACK.
 * Responder: waits for REQ, sends ACK.
 * 
 * Returns:
 *   CEEPEW_OK           - Rendezvous complete (both sides synced)
 *   CEEPEW_ERR_TIMEOUT  - Initiator timed out waiting for ACK
 *   CEEPEW_OK           - In progress (caller should retry)
 *   Other error         - Transport error
 * 
 * Caller must call this repeatedly from session task main loop until
 * CEEPEW_OK (synced) or CEEPEW_ERR_TIMEOUT is returned. */
CeePewErr_t transport_espnow_rendezvous_drive(void)
{
    CeePewErr_t err = CEEPEW_OK;

    /* Check if already synced */
    if (hal_radio_rendezvous_is_synced()) {
        return CEEPEW_OK;
    }

    /* Check for initiator timeout */
    if (hal_radio_rendezvous_check_timeout()) {
        return CEEPEW_ERR_TIMEOUT;
    }

    /* If we're the initiator and haven't sent REQ yet, send it */
    if (session_get_role()) {  /* true = initiator */
        if (!hal_radio_rendezvous_is_synced()) {
            /* Check if we've already sent REQ by checking state */
            /* We'll just try to send - hal_radio_rendezvous_initiator_start
             * will fail if not in IDLE state */
            err = hal_radio_rendezvous_initiator_start();
            if (err != CEEPEW_OK && err != CEEPEW_ERR_PARAM) {
                return err;
            }
            /* CEEPEW_ERR_PARAM means already sent, wait for ACK */
        }
    }

    return CEEPEW_OK;  /* Still in progress */
}
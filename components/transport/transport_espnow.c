/* components/transport/transport_espnow.c */

#include "ceepew_assert.h"
#include "hal_radio.h"
#include "hal_pins.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>

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
    if (s_send_sem != NULL) {
        BaseType_t rc = xSemaphoreGive(s_send_sem);
        (void)rc;
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

    return hal_radio_send(data, len);
}

/* Wait for send-completion event from hal_radio (send callback). This is not a
 * peer-level ACK; it only confirms the local radio stack completed the send
 * operation. Use this as a pragmatic improvement over a no-op ACK stub.
 */
CeePewErr_t transport_wait_ack(const uint8_t *peer_mac, uint8_t seq, uint32_t timeout_ms){
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
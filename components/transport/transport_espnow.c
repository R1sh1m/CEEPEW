/* components/transport/transport_espnow.c */

#include "ceepew_assert.h"
#include "hal_radio.h"
#include "hal_pins.h"
#include "esp_now.h"
#include <stdint.h>

CeePewErr_t transport_espnow_init(void){
    CEEPEW_ASSERT(CEEPEW_ESPNOW_CHANNEL >= 1 && CEEPEW_ESPNOW_CHANNEL <= 13, CEEPEW_ERR_PARAM);
    return CEEPEW_OK;
}

CeePewErr_t transport_espnow_send(const uint8_t *peer_mac, const uint8_t *data, uint16_t len){
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(data != NULL || len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(len <= ESP_NOW_MAX_DATA_LEN, CEEPEW_ERR_BOUNDS);

    CeePewErr_t err = hal_radio_set_peer(peer_mac);
    if (err != CEEPEW_OK){ return err;}
    err = hal_radio_init();
    if (err != CEEPEW_OK) { return err; }
    return hal_radio_send(data, len);
}

/* Stub: wait for ACK for seq from peer within timeout_ms.
 * In a real system this would block/wait for a radio-layer ACK message.
 * For unit tests and targets without an ACK path, this stub returns OK
 * immediately to allow higher layers to test retry logic without a real ACK.
 */
CeePewErr_t transport_wait_ack(const uint8_t *peer_mac, uint8_t seq, uint32_t timeout_ms){
    (void)peer_mac;
    (void)seq;
    (void)timeout_ms;
    return CEEPEW_OK;
}
/* components/ceepew_hal/hal_radio.h */
#ifndef CEEPEW_HAL_RADIO_H
#define CEEPEW_HAL_RADIO_H

#include <stdint.h>
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ceepew_assert.h"

/* RadioFrame_t: Raw ESP-NOW frame received from peer.
 *
 * This structure is posted to the RX queue by the radio_recv_cb() ISR.
 * All fields are populated before queue post — no further validation needed
 * by the receiver.
 *
 * Queue semantics:
 *   - Depth: 8 frames (CEEPEW_QUEUE_DEPTH)
 *   - Thread-safe: ISR posts via xQueueSendFromISR(); task receives via xQueueReceive()
 *   - On queue full: oldest frame is dropped (LIFO eviction), no error returned to ISR
 *   - Frames are NOT acknowledged; loss is transparent to ESP-NOW layer
 */
typedef struct {
    uint8_t   src_mac[6];                     /* Source MAC address                    */
    uint8_t   dst_mac[6];                     /* Destination MAC address               */
    uint8_t   channel;                        /* WiFi channel frame arrived on         */
    int8_t    rssi;                           /* Received signal strength indicator    */
    uint32_t  timestamp_us;                   /* ESP timer timestamp (microseconds)    */
    uint8_t   payload[ESP_NOW_MAX_DATA_LEN];  /* Raw encrypted payload                 */
    uint16_t  payload_len;                    /* Payload length (1..250)               */
} RadioFrame_t;

CeePewErr_t hal_radio_init(void);
CeePewErr_t hal_radio_set_peer(const uint8_t peer_mac[6]);
CeePewErr_t hal_radio_send(const uint8_t *buf, uint16_t len);
CeePewErr_t hal_radio_set_recv_cb(esp_now_recv_cb_t cb);
CeePewErr_t hal_radio_set_channel(uint8_t channel);

/* Send status callback: optional hook for upper layers to be notified when
 * the low-level ESP-NOW send completes (success or failure). Useful for
 * implementing a transport-layer wait-for-ACK using the radio's send callback.
 */
typedef void (*hal_radio_send_status_cb_t)(esp_now_send_status_t status);
CeePewErr_t hal_radio_set_send_status_cb(hal_radio_send_status_cb_t cb);

/* Nonce counter getter callback: required for channel hopping */
typedef uint64_t (*hal_radio_get_nonce_counter_cb_t)(void);

/* Set crypto context and nonce counter getter for channel hopping */
CeePewErr_t hal_radio_set_hop_context(const void *crypto_ctx, hal_radio_get_nonce_counter_cb_t nonce_getter);

/* hal_radio_get_rx_queue: Return the RX frame queue handle.
 *
 * The queue is created during hal_radio_init() and persists for the lifetime
 * of the radio module. Callers (e.g., session_fsm) use this handle to receive
 * frames via xQueueReceive() after initialization.
 *
 * Return value: QueueHandle_t (non-NULL if hal_radio_init() succeeded).
 *               NULL if hal_radio_init() has not been called.
 *
 * Thread-safe: Can be called from any context (ISR-safe on ESP-IDF).
 */
QueueHandle_t hal_radio_get_rx_queue(void);

#endif /* CEEPEW_HAL_RADIO_H */

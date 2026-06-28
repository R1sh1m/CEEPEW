/* components/ceepew_hal/hal_radio.h */
#ifndef CEEPEW_HAL_RADIO_H
#define CEEPEW_HAL_RADIO_H

#include <stdint.h>
#include "esp_now.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ceepew_assert.h"

/* ──────────────────────────────────────────────────────────────────────────── */
/* Rendezvous Convergence Tolerance                                            */
/*                                                                             */
/* Sub-millisecond tolerance window for clock synchronization. The absolute   */
/* difference between Responder uptime and Initiator baseline must converge    */
/* within this window before channel hopping is permitted. 500 us = 0.5 ms,   */
/* well within the 1 ms ceiling. Convergence requires CONVERGE_SAMPLES        */
/* consecutive within-tolerance measurements to reject transient spikes.      */
/* ──────────────────────────────────────────────────────────────────────────── */
#define CEEPEW_RENDEZVOUS_TOLERANCE_US   500U
#define CEEPEW_RENDEZVOUS_CONVERGE_SAMPLES  3U

/* ──────────────────────────────────────────────────────────────────────────── */
/* Channel Hop Shield Timing                                                   */
/*                                                                             */
/* Pre-hop guard:  ms before esp_wifi_set_channel that ARQ is frozen.          */
/* Post-hop guard: ms after  esp_wifi_set_channel that ARQ remains frozen.     */
/* Total blackout window: 10 + 10 + HW_SETTLE = ~22 ms per hop.               */
/* ──────────────────────────────────────────────────────────────────────────── */
#define CEEPEW_HOP_PRE_SHIELD_MS    10U
#define CEEPEW_HOP_POST_SHIELD_MS   10U

/* RadioFrame_t: Raw ESP-NOW frame received from peer.
 *
 * This structure is posted to the RX queue by the radio_recv_cb() ISR.
 * All fields are populated before queue post — no further validation needed
 * by the receiver.
 *
 * Queue semantics:
 *   - Depth: CEEPEW_QUEUE_DEPTH frames (default 32)
 *   - Thread-safe: ISR posts via xQueueOverwriteFromISR(); task receives via xQueueReceive()
 *   - On queue full: oldest frame is overwritten (FIFO eviction), overrun counter incremented
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
CeePewErr_t hal_radio_espnow_reinit(void);
CeePewErr_t hal_radio_set_peer(const uint8_t peer_mac[6]);
CeePewErr_t hal_radio_set_peer_with_lmk(const uint8_t peer_mac[6], const uint8_t lmk[16]);
CeePewErr_t hal_radio_send(const uint8_t *buf, uint16_t len);
CeePewErr_t hal_radio_send_broadcast(const uint8_t *buf, uint16_t len);
CeePewErr_t hal_radio_set_recv_cb(esp_now_recv_cb_t cb);
void hal_radio_reregister_recv_cb(void);
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

/* hal_radio_set_power_save: Switch WiFi modem power-save mode.
 *
 * PARAMETERS:
 *   ps_mode: WIFI_PS_NONE (active), WIFI_PS_MIN_MODEM (Tier 1 save),
 *            WIFI_PS_MAX_MODEM (aggressive — not recommended for ESP-NOW).
 *
 * USE CASE: Call with WIFI_PS_MIN_MODEM during discovery/idle to save ~15 mA,
 *           and WIFI_PS_NONE during active chat for minimum latency.
 *
 * RETURNS:
 *   CEEPEW_OK — Mode set successfully
 *   CEEPEW_ERR_HW — esp_wifi_set_ps() failed
 */
CeePewErr_t hal_radio_set_power_save(wifi_ps_type_t ps_mode);

/* Start the channel hopping timer. Must be called after hal_radio_set_hop_context()
 * when the session is fully established (Phase 3, sync complete, both keys exchanged).
 * Returns CEEPEW_OK on success, CEEPEW_ERR_HW if timer start fails. */
CeePewErr_t hal_radio_start_channel_hopping(void);

/* Stop the channel hopping task and timer. */
CeePewErr_t hal_radio_stop_channel_hopping(void);

/* Check if ESP-NOW peer is currently registered.
 * Returns true if peer is registered, false otherwise.
 * Safe to call from any context after hal_radio_init(). */
bool hal_radio_is_peer_registered(const uint8_t peer_mac[6]);

/* ──────────────────────────────────────────────────────────────────────────── */
/* Hop Synchronization Callbacks (for ARQ pause/resume)                        */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Hop sync callbacks — called from the channel hopping task on Core 1.
 * These allow the ARQ layer to pause retransmit timers before a hop and
 * resume them after the hop completes, preventing retransmissions during
 * channel transitions when the radio is unavailable.
 *
 * Timing:
 *   pre_hop_cb:  Called CEEPEW_HOP_PRE_SHIELD_MS BEFORE the channel change
 *   post_hop_cb: Called CEEPEW_HOP_POST_SHIELD_MS AFTER the channel change
 *
 * Both callbacks execute in the hop task context (Core 1, priority 2).
 * They must be fast and non-blocking (no vTaskDelay, no queue ops).
 */
typedef void (*hal_radio_pre_hop_cb_t)(void);
typedef void (*hal_radio_post_hop_cb_t)(void);

/* Register hop synchronization callbacks.
 * Call once after hal_radio_init() and before hal_radio_start_channel_hopping().
 * Passing NULL for either callback disables that notification.
 */
CeePewErr_t hal_radio_set_hop_sync_callbacks(hal_radio_pre_hop_cb_t pre_hop_cb,
                                             hal_radio_post_hop_cb_t post_hop_cb);

/* ──────────────────────────────────────────────────────────────────────────── */
/* Rendezvous Phase API (Static Channel Sync before Hopping)                    */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Reset rendezvous state machine. Call after BLE teardown, before starting
 * rendezvous handshake. */
CeePewErr_t hal_radio_rendezvous_reset(void);

/* Initiator: send SYNC_RENDEZVOUS_REQ on static channel.
 * Starts the rendezvous handshake. */
CeePewErr_t hal_radio_rendezvous_initiator_start(void);

/* Handle incoming rendezvous frame (REQ or ACK) on static channel.
 * Call from RX processing when a plain rendezvous frame is received.
 * Returns CEEPEW_OK if frame was a rendezvous frame and handled. */
CeePewErr_t hal_radio_rendezvous_handle_rx(const uint8_t *payload, uint16_t len, const uint8_t src_mac[6]);

/* Check if rendezvous handshake completed successfully on both sides AND
 * the clock synchronization has converged within CEEPEW_RENDEZVOUS_TOLERANCE_US.
 * Returns true only when both the handshake is complete and the sub-millisecond
 * convergence criterion is satisfied. */
bool hal_radio_rendezvous_is_synced(void);

/* Get the timing offset (microseconds) calibrated during rendezvous.
 * Positive = responder clock ahead of initiator. */
int32_t hal_radio_rendezvous_get_offset_us(void);

/* Check if initiator timed out waiting for ACK. Resets state on timeout.
 * Returns true if timeout occurred. */
bool hal_radio_rendezvous_check_timeout(void);

/* ──────────────────────────────────────────────────────────────────────────── */
/* Hop Shield State Query                                                      */
/*                                                                             */
/* Returns true when the channel hopping blackout window is active.            */
/* The ARQ layer should suppress retransmissions while this returns true.      */
/* ──────────────────────────────────────────────────────────────────────────── */
bool hal_radio_hop_shield_active(void);

#endif /* CEEPEW_HAL_RADIO_H */

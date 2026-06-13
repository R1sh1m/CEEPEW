/* components/transport/transport_esl.h
 *
 * CEE-PEW ESP-NOW Security Layer (ESL)
 * Implements CRC, timestamp validation, replay window, and MAC locking.
 *
 * SECURITY CRITICAL: Receive pipeline must enforce strict ordering:
 *   DoS → MAC lock → CRC → FEC → timestamp → replay → decrypt → verify
 * All crypto failures result in SILENT DISCARD (no response).
 */

#ifndef TRANSPORT_ESL_H
#define TRANSPORT_ESL_H

#include <stdint.h>
#include <stdbool.h>
#include "ceepew_assert.h"
#include "ceepew_config.h"

typedef CeePewErr_t (*EslMacCheckFn)(const uint8_t mac[6]);
typedef CeePewErr_t (*EslNonceFn)(void);

/* Register session callbacks used by ESL without introducing a hard
 * transport->session dependency. Both callbacks must be non-NULL.
 */
CeePewErr_t esl_register_callbacks(EslMacCheckFn mac_cb, EslNonceFn nonce_cb);

/* Reset ESL callback registration state. Must be called from session_end()
 * so that a subsequent session can re-register callbacks without hitting
 * the "Cannot re-register" assertion. */
void esl_reset_callbacks(void);

#ifdef __cplusplus
extern "C" {
#endif

/* Wrap plaintext with ESL header + CRC for transmission.
 * On input, frame contains payload; on output, frame = header + payload + CRC.
 * 
 * PARAMETERS:
 *   frame: Payload buffer (will be repositioned to make room for header)
 *   len: In/out: payload length → full frame length
 *   max_len: Maximum allowed frame length
 *
 * RETURNS:
 *   CEEPEW_OK — Frame wrapped successfully
 *   CEEPEW_ERR_NULL_PTR — frame or len is NULL
 *   CEEPEW_ERR_BOUNDS — Output would exceed max_len or payload too large
 */
CeePewErr_t transport_esl_process_outgoing(uint8_t *frame, uint16_t *len, uint16_t max_len);
CeePewErr_t transport_esl_get_last_nonce_counter(uint64_t *nonce_counter_out);

/* Process incoming ESL frame with full security pipeline.
 * Implements: DoS check → MAC lock → CRC → FEC → timestamp → replay → crypto verify.
 * 
 * SECURITY: Auth/replay/decrypt failures produce SILENT DISCARD (no response).
 * Only CRC failures (transport errors) may produce retransmission requests.
 *
 * PARAMETERS:
 *   frame: Received frame with header + payload + CRC (will be stripped on success)
 *   len: In/out: frame length → payload length (header + CRC removed)
 *   peer_mac: Source MAC address from ESP-NOW header (6 bytes)
 *   queue_depth: Current RX queue depth (for DoS threshold detection)
 *
 * RETURNS:
 *   CEEPEW_OK — Frame valid and authenticated; payload extracted
 *   CEEPEW_ERR_TRANSPORT — CRC mismatch or protocol violation (may NACK)
 *   CEEPEW_ERR_REPLAY — Duplicate or out-of-window sequence (silent fail)
 *   CEEPEW_ERR_AUTH_FAIL — Ascon or Ed25519 failure (silent fail)
 *   CEEPEW_ERR_PARAM — Frame too short or invalid peer_mac
 *   CEEPEW_ERR_NULL_PTR — frame, len, or peer_mac is NULL
 */
CeePewErr_t transport_esl_process_incoming(uint8_t *frame, uint16_t *len, const uint8_t peer_mac[6], uint32_t queue_depth);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_ESL_H */

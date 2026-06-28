/* components/transport/transport_esl.h
 *
 * CEE-PEW ESP-NOW Security Layer (ESL)
 * Implements CRC, timestamp validation, replay window, and MAC locking.
 *
 * SECURITY CRITICAL: Receive pipeline order:
 *   DoS → MAC lock → CRC → FEC → timestamp → replay → decrypt → verify
 * All crypto failures = SILENT DISCARD (no response).
 */

#ifndef TRANSPORT_ESL_H
#define TRANSPORT_ESL_H

#include <stdint.h>
#include <stdbool.h>
#include "ceepew_assert.h"
#include "ceepew_config.h"

typedef CeePewErr_t (*EslMacCheckFn)(const uint8_t mac[6]);
typedef CeePewErr_t (*EslNonceFn)(void);

/* Register session callbacks. Both must be non-NULL. */
CeePewErr_t esl_register_callbacks(EslMacCheckFn mac_cb, EslNonceFn nonce_cb);

/* Reset callback state. Call from session_end() for re-registration. */
void esl_reset_callbacks(void);

#ifdef __cplusplus
extern "C" {
#endif

/* Wrap payload with ESL header + CRC. frame buffer must have headroom for header. */
CeePewErr_t transport_esl_process_outgoing(uint8_t *frame, uint16_t *len, uint16_t max_len);
CeePewErr_t transport_esl_get_last_nonce_counter(uint64_t *nonce_counter_out);

/* Process incoming ESL frame with full security pipeline.
 * SECURITY: Auth/replay/decrypt failures = SILENT DISCARD. CRC failures may NACK. */
CeePewErr_t transport_esl_process_incoming(uint8_t *frame, uint16_t *len,
                                           const uint8_t peer_mac[6], uint32_t queue_depth);

/* PFS (Perfect Forward Secrecy) handshake frame types */
#define CEEPEW_ESL_MSG_TYPE_MASK       0x0FU
#define CEEPEW_ESL_MSG_TYPE_DATA       0x00U
#define CEEPEW_ESL_MSG_TYPE_PFS_INIT   0x01U  /* PFS public key from initiator */
#define CEEPEW_ESL_MSG_TYPE_PFS_RESP   0x02U  /* PFS public key from responder */

/* Rendezvous phase frame types — sent on static baseline channel before hopping */
#define CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_REQ  0x03U  /* Initiator → Responder: contains uptime */
#define CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_ACK  0x04U  /* Responder → Initiator: contains uptime diff */

/* Extract message type from ESL frame payload (first byte after header) */
CeePewErr_t transport_esl_peek_msg_type(const uint8_t *frame, uint16_t len, uint8_t *msg_type_out);

/* Reset the WireGuard-style 64-bit replay window. Called from
 * session_reset_to_discovery() at the start of each new pairing session
 * so stale seq numbers from prior sessions don't poison the window. */
void transport_replay_reset(void);

/* Process PFS handshake frame — bypasses normal crypto pipeline.
 * Returns CEEPEW_OK on success, peer PFS public key in peer_pfs_pubkey_out.
 * The caller must then call session_pfs_process_peer_key(). */
CeePewErr_t transport_esl_process_pfs_handshake(const uint8_t *frame, uint16_t len,
                                                 const uint8_t peer_mac[6],
                                                 uint8_t peer_pfs_pubkey_out[32]);

/* Build PFS handshake frame (initiator or responder).
 * pfs_pubkey: local PFS public key to send.
 * is_initiator: true for PFS_INIT, false for PFS_RESP.
 * frame buffer must have headroom for ESL header + 1 byte type + 32 bytes pubkey + CRC. */
CeePewErr_t transport_esl_build_pfs_handshake(uint8_t *frame, uint16_t *len, uint16_t max_len,
                                               const uint8_t pfs_pubkey[32], bool is_initiator);

/* ────────────────────────────────────────────────────────────────────── */
/* Rendezvous Phase (Static Channel Sync before Channel Hopping)         */
/* ────────────────────────────────────────────────────────────────────── */

/* Rendezvous frame payload (no ESL header — plaintext on static channel):
 *   REQ:  [0x03][uptime_us_lo][uptime_us_hi] = 1 + 4 + 4 = 9 bytes
 *   ACK:  [0x04][uptime_us_lo][uptime_us_hi][offset_us_lo][offset_us_hi] = 1 + 4 + 4 + 4 + 4 = 17 bytes
 * These are sent as raw ESP-NOW frames on the fixed CEEPEW_ESPNOW_CHANNEL
 * before channel hopping starts. No encryption, no ESL framing.
 */

/* Build rendezvous request frame (initiator).
 * frame: output buffer (min 9 bytes)
 * len: output length written
 * max_len: capacity of frame buffer
 * Returns CEEPEW_OK on success. */
CeePewErr_t transport_esl_build_rendezvous_req(uint8_t *frame, uint16_t *len, uint16_t max_len);

/* Build rendezvous ACK frame (responder).
 * req_uptime: uptime from received REQ frame (microseconds)
 * frame: output buffer (min 17 bytes)
 * len: output length written
 * max_len: capacity of frame buffer
 * Returns CEEPEW_OK on success. */
CeePewErr_t transport_esl_build_rendezvous_ack(uint64_t req_uptime, uint8_t *frame, uint16_t *len, uint16_t max_len);

/* Parse rendezvous request frame.
 * Returns CEEPEW_OK on success, uptime in req_uptime_out (microseconds). */
CeePewErr_t transport_esl_parse_rendezvous_req(const uint8_t *frame, uint16_t len, uint64_t *req_uptime_out);

/* Parse rendezvous ACK frame.
 * Returns CEEPEW_OK on success, uptime diff in offset_us_out (microseconds). */
CeePewErr_t transport_esl_parse_rendezvous_ack(const uint8_t *frame, uint16_t len, int64_t *offset_us_out);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_ESL_H */

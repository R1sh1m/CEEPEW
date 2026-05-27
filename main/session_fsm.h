/* main/session_fsm.h
 *
 * CEE-PEW Session Finite State Machine (3-phase pairing with nonce enforcement)
 *
 * Public API for session lifecycle management:
 * - Phase 1 (DISCOVERY): Initialize session, discover peer
 * - Phase 2 (PAIRING): Exchange session code, derive shared secret
 * - Phase 3 (ACTIVE): Encrypted communication with nonce enforcement
 *
 * All key material is held in volatile memory and securely zeroed on session_end().
 *
 * THREAD SAFETY: Stateless in public API; caller responsible for synchronization
 * if multiple tasks access session state simultaneously (use FreeRTOS mutex).
 */

#ifndef SESSION_FSM_H
#define SESSION_FSM_H

#include <stdint.h>
#include <stdbool.h>
#include "ceepew_assert.h"
#include "ceepew_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────── */
/* Phase 1: Discovery (no encryption)                                   */
/* ────────────────────────────────────────────────────────────────────── */

/* Initialize session FSM with this device's MAC address.
 *
 * PARAMETERS:
 *   device_id: 6-byte MAC address (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Session initialized, phase = 1 (DISCOVERY)
 *   CEEPEW_ERR_NULL_PTR — device_id is NULL
 */
CeePewErr_t session_phase1_init(const uint8_t device_id[6]);

/* Accept peer MAC during discovery phase.
 *
 * PARAMETERS:
 *   peer_mac: 6-byte peer MAC address (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Peer accepted
 *   CEEPEW_ERR_PARAM — Not in phase 1
 *   CEEPEW_ERR_NULL_PTR — peer_mac is NULL
 */
CeePewErr_t session_phase1_accept_peer(const uint8_t peer_mac[6]);

/* ────────────────────────────────────────────────────────────────────── */
/* Phase 2: Pairing (session code + key derivation)                     */
/* ────────────────────────────────────────────────────────────────────── */

/* Initiate pairing phase with human-verified session code.
 * Derives session_id and generates ephemeral Ed25519 keypair.
 *
 * PARAMETERS:
 *   session_code: 32-byte pairing code (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Phase 2 initiated, ready for key derivation
 *   CEEPEW_ERR_PARAM — Not in phase 1
 *   CEEPEW_ERR_NULL_PTR — session_code is NULL
 *   CEEPEW_ERR_CRYPTO — Ed25519 keypair generation failed
 */
CeePewErr_t session_phase2_initiate(const uint8_t session_code[32]);

/* Derive shared secret and transition to Phase 3 (ACTIVE).
 * Implements full RFC 5869 HKDF with digital_sum preprocessing.
 * All intermediate values are securely zeroed.
 *
 * RETURNS:
 *   CEEPEW_OK — Key derivation complete, phase = 3 (ACTIVE)
 *   CEEPEW_ERR_PARAM — Not in phase 2
 *   CEEPEW_ERR_CRYPTO — HKDF or crypto operation failed
 */
CeePewErr_t session_phase2_derive_key(void);

/* ────────────────────────────────────────────────────────────────────── */
/* Phase 3: Active Session (nonce enforcement, encrypted comm)           */
/* ────────────────────────────────────────────────────────────────────── */

/* Enforce nonce limit before every encryption operation.
 * SECURITY: Must be called BEFORE every call to crypto_box or crypto_ascon_aead_encrypt.
 * Increments nonce_counter AFTER checking limit.
 *
 * RETURNS:
 *   CEEPEW_OK — Nonce counter incremented, safe to encrypt
 *   CEEPEW_ERR_NONCE_EXHAUSTED — Nonce counter >= CEEPEW_NONCE_HARD_LIMIT
 *   CEEPEW_ERR_PARAM — Not in phase 3 or session not active
 */
CeePewErr_t session_enforce_nonce_limit(void);

/* Get current nonce for encryption (XSalsa20 format).
 * nonce[0:8] = session_id, nonce[8:16] = nonce_counter (LE), nonce[16:24] = 0
 *
 * PARAMETERS:
 *   nonce: Output buffer for 24-byte XSalsa20 nonce (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Nonce populated
 *   CEEPEW_ERR_PARAM — Not in phase 3 or session not active
 *   CEEPEW_ERR_NULL_PTR — nonce is NULL
 */
CeePewErr_t session_get_nonce(uint8_t nonce[24]);

/* Get Ascon-128 session key (16 bytes, locked after Phase 2).
 *
 * PARAMETERS:
 *   key: Output buffer for 16-byte Ascon key (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Session key populated
 *   CEEPEW_ERR_PARAM — Not in phase 3 or session not active
 *   CEEPEW_ERR_NULL_PTR — key is NULL
 */
CeePewErr_t session_get_session_key(uint8_t key[16]);

/* Sign message with ephemeral Ed25519 private key.
 *
 * PARAMETERS:
 *   msg: Message to sign (may be NULL if msg_len == 0)
 *   msg_len: Message length (0 to CEEPEW_MAX_MSG_BYTES)
 *   sig: Output buffer for 64-byte Ed25519 signature (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Message signed
 *   CEEPEW_ERR_PARAM — Not in phase 3 or session not active
 *   CEEPEW_ERR_NULL_PTR — sig is NULL (or msg is NULL when msg_len > 0)
 *   CEEPEW_ERR_CRYPTO — Signing failed
 */
CeePewErr_t session_sign_message(const uint8_t *msg, uint32_t msg_len, uint8_t sig[64]);

/* Encrypt, frame, sign, and transmit a plaintext message to the peer. */
CeePewErr_t session_send_message(const uint8_t *plaintext, uint16_t len,
                                 const uint8_t peer_mac[6],
                                 const uint8_t peer_public_key[32]);

/* Get peer's Ed25519 public key (for signature verification).
 * In full implementation, exchanged during Phase 2.
 *
 * PARAMETERS:
 *   peer_pk: Output buffer for 32-byte Ed25519 public key (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Peer public key populated
 *   CEEPEW_ERR_PARAM — Not in phase 3 or session not active
 *   CEEPEW_ERR_NULL_PTR — peer_pk is NULL
 */
CeePewErr_t session_get_peer_public_key(uint8_t peer_pk[32]);

/* MAC-locking security check (constant-time).
 * Verify frame came from the bound peer (silent discard on mismatch).
 *
 * PARAMETERS:
 *   peer_mac: Source MAC from received frame (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Frame is from correct peer
 *   CEEPEW_ERR_TRANSPORT — MAC mismatch (silent; indicates attack)
 *   CEEPEW_ERR_PARAM — Not in phase 3 or session not active
 *   CEEPEW_ERR_NULL_PTR — peer_mac is NULL
 */
CeePewErr_t session_mac_lock_check(const uint8_t peer_mac[6]);

/* ────────────────────────────────────────────────────────────────────── */
/* Session Termination                                                   */
/* ────────────────────────────────────────────────────────────────────── */

/* End session and securely zero all key material.
 * Safe to call at any time; idempotent.
 *
 * RETURNS:
 *   CEEPEW_OK — Session ended and cleaned up
 */
CeePewErr_t session_end(void);

/* Phase 4: Secure wipe triggered by TTL expiry or nonce exhaustion.
 * Securely zeros all key material, resets region allocator,
 * clears OLED display, and resets UI state machine to discovery.
 *
 * RETURNS:
 *   CEEPEW_OK — Session wiped, ready for new session
 */
CeePewErr_t session_wipe(void);

/* Phase 4: Track message activity timestamp (for TTL auto-wipe).
 * Call after successfully receiving or sending a message.
 * Used to determine if CEEPEW_MESSAGE_TTL_S has elapsed.
 *
 * RETURNS:
 *   CEEPEW_OK — Timestamp updated
 *   CEEPEW_ERR_PARAM — Not in phase 3
 */
CeePewErr_t session_update_last_message_time(void);

/* Phase 4: Get seconds since last message activity (for TTL check).
 * Returns (current_time_s - last_message_time_s).
 *
 * PARAMETERS:
 *   idle_seconds: Output buffer for idle time in seconds (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Idle time populated
 *   CEEPEW_ERR_PARAM — Not in phase 3
 *   CEEPEW_ERR_NULL_PTR — idle_seconds is NULL
 */
CeePewErr_t session_get_idle_seconds(uint32_t *idle_seconds);

/* Phase 4: Compute device fingerprint from peer public key.
 * fingerprint = SHA256(peer_public_key || device_id)[0:15]
 * Call after successful Ed25519 verify in Phase 3.
 *
 * PARAMETERS:
 *   peer_pk: Peer's Ed25519 public key (32 bytes, not NULL)
 *   device_id: This device's MAC address (6 bytes, not NULL)
 *   fingerprint_out: Output buffer for 16-byte fingerprint (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Fingerprint computed and stored in session context
 *   CEEPEW_ERR_NULL_PTR — Any parameter is NULL
 *   CEEPEW_ERR_CRYPTO — SHA256 computation failed
 */
CeePewErr_t session_compute_fingerprint(const uint8_t peer_pk[32],
                                        const uint8_t device_id[6],
                                        uint8_t fingerprint_out[16]);

/* Phase 4: Get stored fingerprint (for UI confirmation panel).
 *
 * PARAMETERS:
 *   fingerprint: Output buffer for 16-byte fingerprint (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Fingerprint populated
 *   CEEPEW_ERR_PARAM — Not in phase 3 or session not active
 *   CEEPEW_ERR_NULL_PTR — fingerprint is NULL
 */
CeePewErr_t session_get_fingerprint(uint8_t fingerprint[16]);

/* Phase 4: Get local device ID (for fingerprint derivation and UI binding). */
CeePewErr_t session_get_device_id(uint8_t device_id[6]);

/* ────────────────────────────────────────────────────────────────────── */
/* Query/Diagnostic Functions (safe to call at any time)                 */
/* ────────────────────────────────────────────────────────────────────── */

/* Get current session phase (0=idle, 1=discovery, 2=pairing, 3=active) */
uint8_t session_get_phase(void);

/* Check if session is active and ready for encryption */
bool session_is_active(void);

/* Get current nonce counter (for diagnostics; incremented by enforce_nonce_limit) */
uint64_t session_get_nonce_counter(void);

/* Get session ID (derived from peer MAC + timestamp + self MAC) */
uint64_t session_get_id(void);

/* Get 16-byte cryptographic commitment digest of session code (SHA256 truncated).
 * This is used during pairing to cryptographically verify both devices have
 * the same session code. Available in Phase 2 and Phase 3.
 *
 * PARAMETERS:
 *   commitment: Output buffer for 16-byte commitment digest (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Commitment populated
 *   CEEPEW_ERR_PARAM — Not in phase 2 or 3
 *   CEEPEW_ERR_NULL_PTR — commitment is NULL
 */
CeePewErr_t session_get_commitment(uint8_t commitment[CEEPEW_COMMITMENT_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_FSM_H */

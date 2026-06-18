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
 * Increments nonce_counter by 2 AFTER checking limit, preserving parity:
 *   initiator uses even nonces (0,2,4,...), responder uses odd (1,3,5,...).
 * Both nonce sequences are guaranteed disjoint — no collision possible.
 *
 * RETURNS:
 *   CEEPEW_OK — Nonce counter incremented, safe to encrypt
 *   CEEPEW_ERR_NONCE_NEARLY_EXHAUSTED — Counter >= 90% of hard limit
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
 * Returns the key that was stored via session_set_peer_public_key() during
 * the BLE commitment exchange. No derivation or RNG is performed.
 *
 * PARAMETERS:
 *   peer_pk: Output buffer for 32-byte Ed25519 public key (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Peer public key populated
 *   CEEPEW_ERR_PARAM — Not in phase 2+, session not active, or peer's key has
 *                      not yet been received via the BLE commitment exchange
 *   CEEPEW_ERR_NULL_PTR — peer_pk is NULL
 */
CeePewErr_t session_get_peer_public_key(uint8_t peer_pk[32]);

/* Store the peer's ephemeral Ed25519 public key, received over BLE during
 * the Phase 2 commitment exchange. Must be called before
 * session_get_peer_public_key() will return a valid key. Idempotent.
 *
 * PARAMETERS:
 *   peer_pk: 32-byte peer's ephemeral Ed25519 public key (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Stored
 *   CEEPEW_ERR_PARAM — Not in phase 2 or 3
 *   CEEPEW_ERR_NULL_PTR — peer_pk is NULL
 */
CeePewErr_t session_set_peer_public_key(const uint8_t peer_pk[32]);

/* Copy the local ephemeral Ed25519 public key (generated at session_phase2_initiate)
 * into pk_out. Used by the transport layer to publish our sign_pk to the peer
 * over BLE during the commitment exchange. Available in phase 2 and 3.
 *
 * PARAMETERS:
 *   pk_out: Output buffer for 32-byte local sign_pk (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Local sign_pk populated
 *   CEEPEW_ERR_PARAM — Not in phase 2 or 3
 *   CEEPEW_ERR_NULL_PTR — pk_out is NULL
 */
CeePewErr_t session_get_local_sign_pk(uint8_t pk_out[32]);

/* ── X25519 ECDH public key exchange ─────────────────────────────────────── */

/* Get the local ephemeral X25519 public key (generated at session_phase2_derive_key).
 * Used by the transport layer to exchange the ECDH key with the peer over BLE.
 * Available in phase 3.
 *
 * PARAMETERS:
 *   pk_out: Output buffer for 32-byte X25519 public key (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Local box_pubkey populated
 *   CEEPEW_ERR_PARAM — Not in phase 3
 *   CEEPEW_ERR_NULL_PTR — pk_out is NULL
 */
CeePewErr_t session_get_local_box_pubkey(uint8_t pk_out[32]);

/* Store the peer's ephemeral X25519 public key, received over BLE.
 * Must be called before crypto_box encrypt/decrypt will work correctly.
 * Idempotent.
 *
 * PARAMETERS:
 *   peer_pk: 32-byte peer's X25519 public key (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Stored
 *   CEEPEW_ERR_PARAM — Not in phase 2 or 3
 *   CEEPEW_ERR_NULL_PTR — peer_pk is NULL
 */
CeePewErr_t session_set_peer_box_pubkey(const uint8_t peer_pk[32]);

/* Check if the peer's X25519 public key has been received.
 * Returns false until session_set_peer_box_pubkey() has been called. */
bool session_peer_box_pubkey_valid(void);

/* Store the peer's WiFi STA MAC address (6 bytes), received over BLE during
 * the Phase 2 commitment exchange (GATT sign_pk+box_pk+wifi_mac payload).
 * Must be called before session_get_peer_wifi_mac() will return a valid MAC.
 * Idempotent.
 *
 * PARAMETERS:
 *   wifi_mac: 6-byte peer's WiFi STA MAC address (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Stored
 *   CEEPEW_ERR_PARAM — Not in phase 2 or 3
 *   CEEPEW_ERR_NULL_PTR — wifi_mac is NULL
 */
CeePewErr_t session_set_peer_wifi_mac(const uint8_t wifi_mac[6]);

/* Get the peer's WiFi STA MAC address, stored via session_set_peer_wifi_mac().
 * Used by the transport layer for ESP-NOW peer registration.
 *
 * PARAMETERS:
 *   wifi_mac: Output buffer for 6-byte WiFi MAC (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Peer WiFi MAC populated
 *   CEEPEW_ERR_PARAM — Not in phase 2+, session not active, or peer's WiFi MAC
 *                      has not yet been received via the BLE commitment exchange
 *   CEEPEW_ERR_NULL_PTR — wifi_mac is NULL
 */
CeePewErr_t session_get_peer_wifi_mac(uint8_t wifi_mac[6]);

/* Check if the peer's WiFi STA MAC has been received.
 * Returns false until session_set_peer_wifi_mac() has been called. */
bool session_peer_wifi_mac_valid(void);

/* Check if the peer's Ed25519 sign key has been received.
 * Returns false until session_set_peer_public_key() has been called.
 * Both this and session_peer_box_pubkey_valid() MUST return true before
 * the UI is permitted to transition to the chat screen. */
bool session_peer_sign_pk_valid(void);

/* Copy the 32-byte human-verified session code into out. Used by the
 * transport layer to derive the GATT-side sign_pk encryption key (see
 * transport_ble_gatt_crypto.c). The session code is the trust anchor:
 * both peers must enter the same value at pairing time, so the derived
 * key is identical at both ends without any key exchange.
 *
 * Available in phase 2 and 3.
 *
 * PARAMETERS:
 *   out: Output buffer for 32-byte session code (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Session code populated
 *   CEEPEW_ERR_PARAM — Not in phase 2 or 3
 *   CEEPEW_ERR_NULL_PTR — out is NULL
 */
CeePewErr_t session_get_session_code(uint8_t out[32]);

/* Apply a peer-uptime offset (seconds) to the ESL timestamp check.
 * Computed by the transport layer from a one-shot BLE time-sync characteristic
 * read. Calling with 0 clears any applied offset.
 */
CeePewErr_t session_set_peer_uptime_offset(int32_t offset_s);

/* Set the initiator/responder role for this session. Must be called
 * BEFORE session_phase2_derive_key() so the nonce counter starts at the
 * correct parity (initiator: even, responder: odd). Both peers compute
 * the same role from the same MAC ordering — no network exchange needed.
 *
 * PARAMETERS:
 *   initiator: true if local MAC < peer MAC
 *
 * RETURNS:
 *   CEEPEW_OK
 */
CeePewErr_t session_set_role(bool initiator);

/* Get the current role assignment (true = initiator, false = responder).
 * Returns false if role has not been set. */
bool session_get_role(void);

/* Mark the post-derive sync barrier as cleared. Called by the session
 * task after an encrypted MSG_TYPE_KEY_ACK round-trip has been verified.
 * Until this is set, the FSM must NOT transition the UI to CRYPTOGRAM.
 *
 * Idempotent. Returns CEEPEW_OK.
 */
CeePewErr_t session_mark_sync_barrier_cleared(void);

/* Has the post-derive sync barrier been cleared? Used by the session
 * task to gate the UI transition to CRYPTOGRAM. */
bool session_sync_barrier_cleared(void);

/* Drive the post-derive sync exchange. Call from the session task main
 * loop while in phase 3 and the sync barrier is uncleared. The initiator
 * retransmits a 1-byte HELLO inside the encrypted tunnel until the
 * responder's ACK arrives or CEEPEW_KEY_SYNC_TIMEOUT_MS elapses.
 *
 * Returns CEEPEW_ERR_TIMEOUT if the deadline has passed (caller should
 * transition to PAIRING_FAILED), CEEPEW_OK otherwise. */
CeePewErr_t session_drive_post_derive_sync(uint64_t now_ms);

/* Called by the session task's RX path when a 1-byte post-derive sync
 * payload is decoded. Routes the byte to the appropriate handler:
 *   CEEPEW_KEY_SYNC_HELLO_BYTE → mark cleared, return CEEPEW_ERR_NEED_TX
 *                                (caller should send the ACK)
 *   CEEPEW_KEY_SYNC_ACK_BYTE   → mark cleared, return CEEPEW_OK
 *   anything else              → return CEEPEW_ERR_PARAM (ignored) */
CeePewErr_t session_handle_key_sync_byte(uint8_t magic_byte);

/* Read the currently-applied peer-uptime offset (seconds).
 * Returns 0 if no offset has been set.
 */
int32_t session_get_peer_uptime_offset(void);

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

/* Reset session FSM from pairing phase back to discovery (phase 1).
 * Calls session_end() to securely zero all key material, then
 * re-initialises Phase 1 with the stored local device ID so the
 * device is immediately ready to re-discover and re-pair.
 * Safe to call from any phase; idempotent when phase == 1.
 *
 * RETURNS:
 *   CEEPEW_OK  — Session reset to discovery
 *   CEEPEW_ERR — Session phase unknown (pre-1, or re-init failed)
 */
CeePewErr_t session_reset_to_discovery(void);

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

/* Phase 4: Get local device ID (for UI binding). */
CeePewErr_t session_get_device_id(uint8_t device_id[6]);

/* Get currently accepted peer device ID (MAC address). Returns CEEPEW_OK on success. */
CeePewErr_t session_get_peer_device_id(uint8_t peer_id[6]);

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

/* Return the local commitment concatenated with an Ed25519 signature over the commitment.
 * out_buf must be at least (CEEPEW_COMMITMENT_BYTES + 64) bytes long. out_len is set to
 * the actual length written (either CEEPEW_COMMITMENT_BYTES or CEEPEW_COMMITMENT_BYTES+64).
 */
CeePewErr_t session_get_commitment_with_sig(uint8_t *out_buf, uint8_t *out_len);

/* Verify a peer's commitment which may include an appended Ed25519 signature.
 * peer_data: pointer to received bytes (commitment[16] || optional signature[64])
 * len: total length of peer_data
 * Returns CEEPEW_OK on successful verification (commitment and signature ok)
 */
CeePewErr_t session_verify_peer_commitment_with_sig(const uint8_t *peer_data, uint8_t len);

/* ── Test injection setters (replace the old __attribute__((weak)) mocks) ──
 * Setting a value short-circuits the corresponding session_get_*() return.
 * Safe to call from any context. Marked with a leading underscore on the
 * shadow flag in implementation to make grep cleaner. */
void session_test_set_id(uint64_t id);
void session_test_set_nonce_counter(uint64_t nc);
void session_test_set_commitment(const uint8_t c[CEEPEW_COMMITMENT_BYTES]);
void session_test_unset_commitment(void);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_FSM_H */

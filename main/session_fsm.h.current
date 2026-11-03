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
#include "ceepew_config.h"
#include "hal_ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────── */
/* Compile-time profile / policy switches                                 */
/* ────────────────────────────────────────────────────────────────────── */
/* sdkconfig symbols do not exist in host builds, so they are normalised to
 * 0/1 macros here. Every conditional in this translation unit and in
 * session_send.c reads the normalised name, never CONFIG_* directly. */

/* Lite profile (ISS profile flag): no outer crypto_box, no per-fragment
 * Ed25519 signature. Default 0 = Full stack, byte-identical to the
 * pre-flag build. */
#if defined(CONFIG_CEEPEW_PROFILE_LITE) && CONFIG_CEEPEW_PROFILE_LITE
#define CEEPEW_PROFILE_LITE_ENABLED 1
#else
#define CEEPEW_PROFILE_LITE_ENABLED 0
#endif

/* Unauthenticated-session escape hatch (ISS-S01). Default 0: the degraded
 * pairing path is compiled out and cannot reach ACTIVE. */
#if defined(CONFIG_CEEPEW_ALLOW_UNAUTH_SESSION) && CONFIG_CEEPEW_ALLOW_UNAUTH_SESSION
#define CEEPEW_ALLOW_UNAUTH_SESSION_ENABLED 1
#else
#define CEEPEW_ALLOW_UNAUTH_SESSION_ENABLED 0
#endif

/* Phase 1.3: Authenticated Profile Negotiation bitmask */
#define CEEPEW_PROFILE_MASK_LITE        (1U << 0)
#define CEEPEW_PROFILE_MASK_FEC         (1U << 1)
#define CEEPEW_PROFILE_MASK_ASYM_SIG    (1U << 2)

static inline uint8_t session_get_local_profile_byte(void) {
    uint8_t prof = 0U;
#if CEEPEW_PROFILE_LITE_ENABLED
    prof |= CEEPEW_PROFILE_MASK_LITE;
#else
    prof |= CEEPEW_PROFILE_MASK_ASYM_SIG;
#endif
#if defined(CONFIG_CEEPEW_FEC_DISABLE) && CONFIG_CEEPEW_FEC_DISABLE
    /* FEC disabled */
#else
    prof |= CEEPEW_PROFILE_MASK_FEC;
#endif
    return prof;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Pairing code space and attempt bound (ISS-S02)                         */
/* ────────────────────────────────────────────────────────────────────── */
/* Ground truth read from the implementation, not from the manuscript:
 *
 *   - The user selects CEEPEW_PAIRING_CODE_CHARS = 4 characters with the
 *     potentiometer. The alphabet is CODE_CHARSET in
 *     components/ceepew_hal/ui_manager.c ("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"),
 *     i.e. CEEPEW_PAIRING_CODE_ALPHABET = 36 symbols, uppercase, single case.
 *   - The CONFIRM screen (render_confirm(), ui_manager.c) draws exactly those
 *     four entered bytes in a 2x2 matrix. The displayed code is NOT derived
 *     from anything: displayed == entered. The peer comparison is the
 *     commitment match, not a second code read off the display.
 *   - The 32-byte session code consumed by session_phase2_initiate() is the
 *     four ASCII bytes repeated eight times (task_session_build_session_code()).
 *   - "0000" is rejected (ui_manager.c all-zero guard), so the reachable code
 *     space is CEEPEW_PAIRING_CODE_SPACE - 1.
 *
 * Entropy: log2(36^4) = 20.68 bits of code space, of which the user supplies
 * whatever they choose (no stretching is applied). The online-guess work
 * factor inside one pairing window is bounded by CEEPEW_PAIRING_MAX_ATTEMPTS
 * full BLE+GATT identity cycles, since each attempt costs a discovery and a
 * GATT connection, not a fast local test. */
#define CEEPEW_PAIRING_CODE_CHARS        4U
#define CEEPEW_PAIRING_CODE_ALPHABET     36U    /* 0-9, A-Z */
#define CEEPEW_PAIRING_CODE_SPACE        1679616UL /* 36^4, exactly */
#define CEEPEW_PAIRING_MAX_ATTEMPTS      10U    /* online attempts per boot */

/* ────────────────────────────────────────────────────────────────────── */
/* Peer identity policy (ISS-S01, fail-closed)                            */
/* ────────────────────────────────────────────────────────────────────── */
/* The three facts that together mean "the peer's Ed25519 sign_pk is
 * verified". They are kept as a plain POD so the policy below is compiled
 * and unit-tested on the host without pulling in ESP-IDF, while the
 * firmware feeds it from the live session context.
 *
 *   sign_pk_received : session_set_peer_public_key() accepted a 32-byte key
 *   sign_pk_format_ok: that key is a canonical Ed25519 encoding (RFC 8032)
 *   commitment_bound : the peer commitment matched our locally computed
 *                      commitment AND the key arrived in the GATT payload
 *                      sealed under the session-code-derived AEAD key
 */
typedef struct {
    bool sign_pk_received;
    bool sign_pk_format_ok;
    bool commitment_bound;
} SessionPeerIdentity_t;

/**
 * @brief Canonical-encoding check for a 32-byte Ed25519 public key.
 *
 * Rejects the all-zero key and any non-canonical y (y >= 2^255-19, RFC 8032
 * §5.1.3). This is a format check only; cryptographic validity is
 * established by the signature verification itself.
 *
 * @param pk 32-byte candidate key (NULL fails closed)
 * @return true when the encoding is acceptable
 */
static inline bool session_sign_pk_format_ok(const uint8_t pk[32])
{
    static const uint8_t p_le[32] = {
        0xEDU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x7FU
    };
    if (pk == NULL) { return false; }

    uint8_t nonzero = 0U;
    for (uint8_t i = 0U; i < 32U; i++) { nonzero |= pk[i]; }
    if (nonzero == 0U) { return false; }

    /* Compare y (sign bit masked) against p, most-significant byte first. */
    for (int i = 31; i >= 0; i--) {
        uint8_t a = pk[i];
        if (i == 31) { a = (uint8_t)(a & 0x7FU); }
        if (a < p_le[i]) { return true; }
        if (a > p_le[i]) { return false; }
    }
    return false;  /* y == p is not a canonical encoding */
}

/**
 * @brief Length gate for a received Ed25519 public key.
 * @param pk_len Received length in bytes; must be exactly 32.
 */
static inline bool session_peer_pk_length_ok(uint16_t pk_len)
{
    return (pk_len == 32U);
}

/**
 * @brief Is the peer identity verified? Fail-closed on a NULL argument.
 */
static inline bool session_peer_identity_is_verified(const SessionPeerIdentity_t *id)
{
    if (id == NULL) { return false; }
    return id->sign_pk_received && id->sign_pk_format_ok && id->commitment_bound;
}

/**
 * @brief May the pairing FSM advance to ACTIVE?
 *
 * @param id                peer identity facts for this pairing
 * @param degraded          true when the DEGRADED path was entered
 * @param allow_unauth      true only in a CONFIG_CEEPEW_ALLOW_UNAUTH_SESSION build
 *
 * Without the escape hatch a degraded pairing can never activate, whatever
 * the user confirmed on the degraded screen: DEGRADED is terminal.
 */
static inline bool session_pairing_may_activate(const SessionPeerIdentity_t *id,
                                                bool degraded,
                                                bool allow_unauth)
{
    if (session_peer_identity_is_verified(id)) { return true; }
    if (allow_unauth && degraded) { return true; }
    return false;
}

/**
 * @brief Transmit gate shared by the session send path.
 *
 * @param identity_verified result of session_peer_identity_verified()
 * @return CEEPEW_OK, or CEEPEW_ERR_AUTH_FAIL when identity is unverified
 */
static inline CeePewErr_t session_tx_identity_gate(bool identity_verified)
{
    return identity_verified ? CEEPEW_OK : CEEPEW_ERR_AUTH_FAIL;
}

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

/* Roll back the nonce counter by 2 after a TX pipeline failure.
 *
 * SECURITY INVARIANT: Call ONLY when session_enforce_nonce_limit() succeeded
 * but the encrypted frame was NOT transmitted (pipeline failed before
 * ecc_arq_send()). Rolling back after a frame has been sent — even if no
 * ACK was received — would replay a nonce the peer's ESL replay window has
 * already accepted, enabling a trivial replay attack. Do NOT call this on
 * ARQ timeout (the frame may have been received by the peer).
 *
 * Preserves parity: decrements by 2, keeping the counter on the same
 * even (initiator) or odd (responder) track.
 *
 * RETURNS:
 *   CEEPEW_OK        — Counter decremented successfully
 *   CEEPEW_ERR_PARAM — Not in phase 3, session inactive, or counter < 2
 */
CeePewErr_t session_rollback_nonce(void);


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

#if CONFIG_CEEPEW_BENCH_58B
/* Bench-only: arm corruption of the next frame's outer box ciphertext
 * (post-encrypt, pre-sign), exercising the peer's forged-fresh-seq reject
 * path (box HMAC fail) over the air. No effect unless armed. */
void session_send_bench_forge_next(void);

/* Bench-only: arm replay injection of the next frame's ESL-wrapped blob
 * (stale ESL msg_id, fresh ARQ seq downstream), exercising the peer's
 * replay-window reject path (CEEPEW_ERR_REPLAY) over the air. No effect
 * unless armed. */
void session_send_bench_replay_next(void);
#endif

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

/* Store this device's own WiFi STA MAC address (6 bytes).
 * Must be called before session_phase2_derive_key() to enable transport
 * identity binding. The local and peer WiFi MACs are sorted and bound
 * into the HKDF info for symmetric key convergence.
 *
 * PARAMETERS:
 *   wifi_mac: 6-byte local WiFi STA MAC address (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Stored
 *   CEEPEW_ERR_NULL_PTR — wifi_mac is NULL
 */
CeePewErr_t session_set_self_wifi_mac(const uint8_t wifi_mac[6]);

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
 * the UI is permitted to transition to the chat screen.
 *
 * NOTE: receipt is not verification. Use session_peer_identity_verified()
 * for the fail-closed identity predicate that gates transmission. */
bool session_peer_sign_pk_valid(void);

/* Fail-closed peer identity predicate (ISS-S01).
 *
 * True only when all three hold:
 *   1. a peer Ed25519 sign_pk was received over the GATT identity payload,
 *   2. it passed the canonical-encoding format check, and
 *   3. it is bound to the pairing commitment, i.e. the peer commitment
 *      matched the locally computed one and the key arrived inside the
 *      session-code-sealed payload (both facts require the same human code).
 *
 * Every application-data transmit path must refuse with CEEPEW_ERR_AUTH_FAIL
 * when this returns false. Cleared by session_end()/session_wipe() and by
 * every session_phase1_init(). */
bool session_peer_identity_verified(void);

/* True when this session may transmit application data.
 *
 * Composed from the SAME predicate that gates the pairing → ACTIVE
 * transition (session_pairing_may_activate), so "reached ACTIVE" and
 * "may send" cannot drift apart. In a default build this is exactly
 * session_peer_identity_verified(); in a CONFIG_CEEPEW_ALLOW_UNAUTH_SESSION
 * build it additionally re-admits a user-confirmed degraded session. */
bool session_tx_permitted(void);

/* Length-checked peer Ed25519 sign_pk setter.
 *
 * Behaves like session_set_peer_public_key() but rejects any length other
 * than 32 bytes with CEEPEW_ERR_BOUNDS and leaves the stored identity
 * unverified. The 32-byte wrapper above delegates here, so a production
 * caller and the fail-closed host test take the same path.
 *
 * PARAMETERS:
 *   peer_pk:     pointer to the received key bytes (not NULL)
 *   peer_pk_len: received length in bytes (must be 32)
 *
 * RETURNS:
 *   CEEPEW_OK — Stored (still subject to commitment binding)
 *   CEEPEW_ERR_NULL_PTR / CEEPEW_ERR_BOUNDS / CEEPEW_ERR_PARAM
 */
CeePewErr_t session_set_peer_public_key_checked(const uint8_t *peer_pk,
                                                uint16_t peer_pk_len);

/* Number of pairing attempts started since boot / last successful session.
 * Incremented once per session_phase2_initiate() and bounded by
 * CEEPEW_PAIRING_MAX_ATTEMPTS; the counter is reset when key derivation
 * reaches ACTIVE and on session_wipe(). Exposed for the status screen and
 * for the runtime log line that reports "attempt n/limit". */
uint32_t session_get_pairing_attempt_count(void);

/* Mark the session as identity-degraded (peer sign_pk was never received
 * but the user explicitly confirmed they wish to proceed without identity
 * verification).
 *
 * ISS-S01: without CONFIG_CEEPEW_ALLOW_UNAUTH_SESSION=y this no longer
 * unlocks a session. The flag is still recorded so the UI can show the
 * UNVERIFIED IDENTITY banner and offer a retry, but DEGRADED is terminal:
 * session_phase2_derive_key() refuses to advance to ACTIVE and the transmit
 * path refuses every frame. In an escape-hatch build the legacy permissive
 * behaviour is restored verbatim so the gate can be exercised on the host.
 *
 * Must be called before session_phase2_derive_key(). */
void session_set_identity_degraded(void);

/* Returns true if the current session is operating in identity-degraded
 * mode (no peer Ed25519 sign_pk available, user-confirmed fallback). */
bool session_is_identity_degraded(void);

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

/* Responder-side confirmation: call AFTER the ACK frame has been
 * successfully enqueued for transmission following a HELLO receipt.
 * This completes the double-ended rendezvous — the barrier is only
 * cleared when both peer proof (HELLO received) and local proof
 * (ACK sent) are confirmed. Idempotent. */
CeePewErr_t session_confirm_ack_sent(void);

/* Verify that a received ESP-NOW frame's source MAC matches the peer
 * WiFi STA MAC that was delivered over the secure GATT channel.
 * Returns CEEPEW_OK on match, CEEPEW_ERR_AUTH_FAIL on mismatch.
 * Used during the post-derive sync exchange for transport binding. */
CeePewErr_t session_verify_wifi_mac_matches_frame(const uint8_t frame_src_mac[6]);

/* ────────────────────────────────────────────────────────────────────── */
/* Rendezvous Phase (Static Channel Sync before Channel Hopping)         */
/* ────────────────────────────────────────────────────────────────────── */

/* Initialize rendezvous state. Call once after key derivation and BLE teardown,
 * before starting the rendezvous handshake on the static channel.
 * Returns CEEPEW_OK on success. */
CeePewErr_t session_rendezvous_init(void);

/* Drive rendezvous handshake from session task.
 * Called repeatedly from session task main loop after ESP-NOW init but before
 * starting channel hopping. The rendezvous handshake runs on the static
 * CEEPEW_ESPNOW_CHANNEL using plaintext frames (no encryption).
 * 
 * Returns:
 *   CEEPEW_OK          - Rendezvous complete (synced, ready for hopping)
 *   CEEPEW_ERR_TIMEOUT - Initiator timed out waiting for ACK
 *   CEEPEW_OK          - In progress (caller should retry)
 *   Other error        - Transport error
 * 
 * The rendezvous must complete before hal_radio_start_channel_hopping() is called. */
CeePewErr_t session_drive_rendezvous(void);

/* Check if rendezvous handshake is complete and both sides are synced. */
bool session_rendezvous_synced(void);

/* Get the rendezvous timing offset (microseconds).
 * Positive = responder clock ahead of initiator.
 * Used to calibrate channel hopping timer phase alignment. */
int32_t session_get_rendezvous_offset_us(void);

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

/* Get nonce counter for UI display (cross-core safe).
 * Unlike session_get_nonce_counter(), this function reads a volatile snapshot
 * that the session task updates after every increment. Safe to call from
 * Core 0 (UI task) without a mutex. */
uint64_t session_get_nonce_counter_display(void);

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

/* Send a message and wait for an echo response (round-trip test).
 * The peer must be running the same test and will echo the payload back.
 * Returns CEEPEW_OK on successful round-trip, error on timeout or failure. */
CeePewErr_t session_send_roundtrip(const uint8_t *payload, uint16_t len, uint32_t timeout_ms);

/* ────────────────────────────────────────────────────────────────────── */
/* PFS (Perfect Forward Secrecy) — ephemeral ECDH over ESP-NOW           */
/* ────────────────────────────────────────────────────────────────────── */

/* Generate local ephemeral PFS keypair and store in crypto context.
 * Called once after ESP-NOW link is established (post LMK peer registration).
 * The public key will be sent to the peer via a dedicated PFS handshake frame. */
CeePewErr_t session_pfs_initiate(void);

/* Process peer's PFS public key and derive shared secret + session Ascon key.
 * Called when a PFS handshake frame is received from the peer.
 * On success, the PFS-derived Ascon key becomes the active session key. */
CeePewErr_t session_pfs_process_peer_key(const uint8_t peer_pfs_pubkey[32]);

/* Check if PFS handshake is complete and PFS-derived key is active. */
bool session_pfs_active(void);

/* Deferred PFS activation: call from the session task main loop to
 * activate PFS when the peer key has arrived (pfs_peer_pubkey_valid)
 * but activation has not yet occurred (!pfs_active). This is the
 * symmetric deferred path — both sides activate independently in their
 * own main loops rather than one side activating immediately in the
 * RX callback, which could cause one-sided key mismatch if PFS_RESP
 * is lost and the initiator falls back to the base key.
 * Safe no-op if PFS is already active or not ready. */
void session_pfs_check_activate(void);

/* Check if the peer's PFS public key has been received and the PFS shared
 * secret has been derived (but not yet activated). Returns true once
 * session_pfs_process_peer_key() has stored the peer's key, regardless of
 * whether session_pfs_activate() has been called. Used by the PFS retry
 * loop to stop retransmission as soon as the peer has acknowledged. */
bool session_pfs_peer_ready(void);

/* Get the PFS ephemeral public key to send to peer.
 * Must be called after session_pfs_initiate() returns success. */
CeePewErr_t session_get_local_pfs_pubkey(uint8_t pfs_pubkey_out[32]);

/* Derive the dedicated HMAC key that authenticates the in-band PFS handshake. */
CeePewErr_t session_pfs_auth_key(uint8_t out[32]);

/* ── Test injection setters (replace the old __attribute__((weak)) mocks) ──
 * Setting a value short-circuits the corresponding session_get_*() return.
 * Safe to call from any context. Marked with a leading underscore on the
 * shadow flag in implementation to make grep cleaner. */
void session_test_set_id(uint64_t id);
void session_test_set_nonce_counter(uint64_t nc);
void session_test_set_commitment(const uint8_t c[CEEPEW_COMMITMENT_BYTES]);
void session_test_unset_commitment(void);
void session_test_set_commitment_verified(bool verified);
void session_test_reset_attempts(void);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_FSM_H */

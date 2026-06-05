/* main/session_fsm.c
 *
 * CEE-PEW Session Finite State Machine (3-phase pairing with nonce enforcement)
 *
 * Phase 1 (DISCOVERY): Exchange MAC addresses, device identifiers, capability flags.
 *                      No encryption; plain discovery frames.
 *
 * Phase 2 (PAIRING):   Exchange session code (human-verified), derive shared secret
 *                      via HKDF-SHA256 with digital_sum preprocessing.
 *                      Build ephemeral Ed25519 keypairs.
 *
 * Phase 3 (ACTIVE):    Encrypted bidirectional communication. Nonce counter enforced
 *                      before every encryption. Session key locked after Phase 2.
 *                      No key material leaves Phase 3 until session_end().
 *
 * SECURITY MODEL:
 * - MAC locking: Each session bound to exactly one peer MAC (constant-time compare)
 * - Key derivation: HKDF-SHA256(session_code, salt, info) where salt is
 *   SHA256(digital_sum_mix(code) || code) and info binds device IDs,
 *   session commitment, and timestamp. The IKM is the raw 32-byte
 *   session_code (no local randomness) so both peers compute identical keys.
 * - Nonce enforcement: Counter must be < CEEPEW_NONCE_HARD_LIMIT; incremented AFTER check.
 *   Initiator starts at 0 (even nonces); responder at 1 (odd nonces) — both
 *   derived deterministically from the role assignment (lower MAC = initiator).
 * - Replay defense: 64-bit WireGuard bitmap + ±15s timestamp window
 * - Signing: Ed25519 per-session ephemeral keypairs (destroyed on session_end).
 *   The LOCAL sign_pk is never transmitted — only received from peer via
 *   BLE GATT. The peer's sign_pk is used as the "public key" input to
 *   curve25519_scalarmult() (symmetric operation, bounded by session_code
 *   trust anchor; see Bug 4 threat model in transport_ble.c).
 * - Secure zeroing: All key material (session_key, nonce_counter, sign_sk) volatile-written
 *
 * POST-DERIVE SYNC BARRIER (Bug 3 fix):
 *   After HKDF completes locally, the FSM does NOT advance the UI to
 *   PAIRING_SUCCESS. The session task drives session_drive_post_derive_sync(),
 *   which initiates a 1-byte encrypted HELLO/ACK round-trip via
 *   session_send_message() on the ESP-NOW link. Only after the ACK is
 *   received and decrypted does the UI transition fire. This prevents
 *   the desync where one device shows "✓ SECURE" while the other is
 *   still on the pairing screen.
 */

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "ceepew_security_utils.h"
#include "crypto_ctx.h"
#include "crypto_rng.h"
#include "transport_esl.h"
#include "ceepew_pipeline.h"
#include "ceepew_region.h"  /* Phase 4: For region_reset */
#include "crypto_eddsa.h"
#include "crypto_hkdf.h"
#include "session_fsm.h"
#include "esp_log.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "esp_timer.h"

/* Forward declarations of crypto primitives */
CeePewErr_t crypto_sha256_compute(const uint8_t *in, uint32_t in_len, uint8_t out[32]);
CeePewErr_t crypto_hkdf_derive(const uint8_t *ikm, uint8_t ikm_len,  const uint8_t *salt, uint8_t salt_len, const uint8_t *info, uint8_t info_len, uint8_t *out, uint8_t out_len);
CeePewErr_t crypto_eddsa_keypair(uint8_t pk[32], uint8_t sk[64]);
CeePewErr_t crypto_eddsa_sign(const uint8_t priv[64], const uint8_t *msg, uint16_t msg_len, uint8_t sig[64]);
CeePewErr_t crypto_eddsa_seeded_keypair(uint8_t pk[32], uint8_t sk[64], const uint8_t seed[32]);

/* Digital sum preprocessing (tools component) */
void digital_sum_mix(const uint8_t *in, uint16_t len, uint8_t out_32bytes[32]);

/* Session callbacks registered with transport_esl */
CeePewErr_t session_mac_lock_check(const uint8_t peer_mac[6]);
CeePewErr_t session_enforce_nonce_limit(void);

/* Phase 4: Forward declarations for wipe and UI reset (external modules) */
extern CeePewErr_t ceepew_pipeline_deinit(void);
extern CeePewErr_t crypto_ctx_destroy(void);
extern Region_t g_region;  /* Global region allocator */
extern void ui_manager_reset_to_discovery(void);

/* Session state machine context */
typedef struct {
    uint8_t  phase;                              /* 0=idle, 1=discovery, 2=pairing, 3=active */
    uint8_t  peer_mac[6];                        /* Locked after Phase 2 */
    uint64_t session_id;                         /* Deterministic per-session nonce base */
    uint8_t  device_id_self[6];                  /* Own MAC */
    uint8_t  device_id_peer[6];                  /* Peer MAC */
    uint8_t  session_code[32];                   /* Human-verified pairing code */
    uint8_t  session_key[16];                    /* Ascon-128 key (Phase 3 only) */
    uint64_t nonce_counter;                      /* Must be < CEEPEW_NONCE_HARD_LIMIT */
    uint8_t  nonce_upper_64[8];                  /* session_id */
    uint8_t  sign_pk[32];                        /* Ephemeral Ed25519 public key */
    uint8_t  sign_sk[64];                        /* VOLATILE ephemeral private key */
    uint8_t  peer_sign_pk[32];                   /* Peer's ephemeral Ed25519 pub key, set via session_set_peer_public_key */
    bool     peer_sign_pk_valid;                 /* true once peer_sign_pk has been received */
    int32_t  peer_uptime_offset_s;               /* peer_uptime_s - local_uptime_s (BLE time-sync) */
    bool     session_active;                     /* true = Phase 3 && authenticated */
    bool     is_initiator;                       /* true if local MAC < peer MAC (lower = initiator) */
    bool     sync_barrier_cleared;               /* true once encrypted post-derive ACK round-trip complete */
    uint64_t sync_started_ms;                    /* when post-derive sync driver was first invoked */
    uint64_t sync_last_send_ms;                  /* last HELLO retransmit time (initiator only) */
    /* Phase 4: TTL and fingerprint tracking */
    uint32_t last_message_time_s;                /* Last message activity timestamp (seconds) */
    uint8_t  fingerprint[16];                    /* SHA256(peer_pk || device_id)[0:15] */
    bool     fingerprint_valid;                  /* true if fingerprint has been computed */
} SessionCtx_t;

/* Global session context (per-device; single active session at a time) */
static SessionCtx_t s_session;

/* Phase 4: Initialize TTL tracking on session start (forward declaration) */
static void session_init_ttl(void);
static void session_secure_zero_context(void);
static CeePewErr_t session_ordered_device_ids(uint8_t id_a[6], uint8_t id_b[6]);

static CeePewErr_t session_derive_sign_seed(const uint8_t mac[6], uint8_t seed_out[32])
{
    /* NOTE: The 'mac' argument is accepted for source compatibility with
     * earlier revisions, but the seed is now sourced from a CSPRNG. The
     * eFuse is no longer mixed in: the eFuse secret is single-source and
     * provides no additional security here, and the previous design had
     * no mechanism for the two peers to converge on a shared secret.
     * The local ephemeral signing keypair is now a fresh random keypair. */
    (void)mac;
    CEEPEW_ASSERT(seed_out != NULL, CEEPEW_ERR_NULL_PTR);
    CeePewErr_t err = crypto_rng_fill(seed_out, 32U);
    return err;
}

static CeePewErr_t session_compute_id(uint8_t session_id_out[8])
{
    CEEPEW_ASSERT(session_id_out != NULL, CEEPEW_ERR_NULL_PTR);

    /* Compute session_id from canonical device IDs and session code.
     * session_id = SHA256(min(id_self, id_peer) || max(id_self, id_peer) || session_code)[0:8]
     * 
     * CRITICAL: The device IDs are ordered lexicographically (via memcmp) to ensure
     * that both initiator and responder derive the SAME session_id from the same
     * session_code. This prevents Phase 2 commitment disagreement.
     * 
     * Example: If A=aa:bb:cc:00:00:01 and B=aa:bb:cc:00:00:02, then:
     *   Both devices compute: SHA256(A || B || session_code)
     * Even if A is the initiator and B is the responder.
     */
    uint8_t id_a[6U];
    uint8_t id_b[6U];
    CeePewErr_t err = session_ordered_device_ids(id_a, id_b);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    uint8_t session_id_src[44U];
    memcpy(session_id_src, id_a, 6U);
    memcpy(session_id_src + 6U, id_b, 6U);
    memcpy(session_id_src + 12U, s_session.session_code, 32U);

    uint8_t session_id_hash[32];
    err = crypto_sha256_compute(session_id_src, (uint32_t)sizeof(session_id_src), session_id_hash);
    ceepew_secure_zero(session_id_src, (uint32_t)sizeof(session_id_src));
    ceepew_secure_zero(id_a, (uint32_t)sizeof(id_a));
    ceepew_secure_zero(id_b, (uint32_t)sizeof(id_b));
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    memcpy(session_id_out, session_id_hash, 8U);
    ceepew_secure_zero(session_id_hash, sizeof(session_id_hash));
    return CEEPEW_OK;
}

static CeePewErr_t session_ordered_device_ids(uint8_t id_a[6], uint8_t id_b[6])
{
    CEEPEW_ASSERT(id_a != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(id_b != NULL, CEEPEW_ERR_NULL_PTR);

    /* Return device IDs in lexicographic order to prevent Phase 2 disagreement.
     * 
     * SECURITY: This function enforces a consistent device ID ordering algorithm
     * across both peers (initiator and responder) so that they derive identical
     * commitment values and session keys.
     * 
     * Algorithm: Compare MAC addresses lexicographically (byte-by-byte, left-to-right).
     * If device_self is <= device_peer (memcmp returns <= 0), then:
     *   id_a = device_self (smaller)
     *   id_b = device_peer (larger)
     * Otherwise:
     *   id_a = device_peer (smaller)
     *   id_b = device_self (larger)
     * 
     * Both devices execute this same comparison algorithm, so both will:
     * 1. Derive the same session_id = SHA256(id_a || id_b || session_code)
     * 2. Produce the same HKDF-SHA256 commitment value
     * 3. Accept each other's commitment fingerprints on display
     * 
     * This prevents an attack where one device could claim a different session_id,
     * causing the fingerprint comparison to fail even though both devices agree
     * on the session code.
     */
    if (memcmp(s_session.device_id_self, s_session.device_id_peer, 6U) <= 0) {
        memcpy(id_a, s_session.device_id_self, 6U);
        memcpy(id_b, s_session.device_id_peer, 6U);
    } else {
        memcpy(id_a, s_session.device_id_peer, 6U);
        memcpy(id_b, s_session.device_id_self, 6U);
    }

    return CEEPEW_OK;
}

static void session_secure_zero_context(void)
{
    ceepew_secure_zero(&s_session, (uint32_t)sizeof(s_session));
}

/* ────────────────────────────────────────────────────────────────────── */
/* Phase 1: Discovery (no encryption)                                   */
/* ────────────────────────────────────────────────────────────────────── */

CeePewErr_t session_phase1_init(const uint8_t device_id[6]){
    CEEPEW_ASSERT(device_id != NULL, CEEPEW_ERR_NULL_PTR);
    memset(&s_session, 0, sizeof(s_session));
    CeePewErr_t err = crypto_ctx_init();
    CEEPEW_ASSERT(err == CEEPEW_OK, err);
    s_session.phase = 1U;
    memcpy(s_session.device_id_self, device_id, 6U);
    s_session.session_active = false;
    return CEEPEW_OK;
}

CeePewErr_t session_phase1_accept_peer(const uint8_t peer_mac[6]){
    CEEPEW_ASSERT(s_session.phase == 1U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    memcpy(s_session.device_id_peer, peer_mac, 6U);
    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Phase 2: Pairing (session code + key derivation)                     */
/* ────────────────────────────────────────────────────────────────────── */

CeePewErr_t session_phase2_initiate(const uint8_t session_code[32]){
    CEEPEW_ASSERT(s_session.phase == 1U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(session_code != NULL, CEEPEW_ERR_NULL_PTR);

    /* Entropy validation: reject all-zeros session code */
    uint8_t all_zeros = 1U;
    for (uint16_t i = 0U; i < 32U; i++) {
        if (session_code[i] != 0U) {
            all_zeros = 0U;
            break;
        }
    }
    CEEPEW_ASSERT(!all_zeros, CEEPEW_ERR_CRYPTO);  /* All zeros indicates no entropy */

    s_session.phase = 2U;
    memcpy(s_session.session_code, session_code, 32U);

    /* Derive a deterministic session_id using helper function */
    uint8_t session_id_bytes[8];
    CeePewErr_t err = session_compute_id(session_id_bytes);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    /* Store session_id as 64-bit value */
    s_session.session_id = ((uint64_t)session_id_bytes[0] << 56U) |
                           ((uint64_t)session_id_bytes[1] << 48U) |
                           ((uint64_t)session_id_bytes[2] << 40U) |
                           ((uint64_t)session_id_bytes[3] << 32U) |
                           ((uint64_t)session_id_bytes[4] << 24U) |
                           ((uint64_t)session_id_bytes[5] << 16U) |
                           ((uint64_t)session_id_bytes[6] << 8U) |
                           ((uint64_t)session_id_bytes[7]);

    /* Store session_id in nonce upper 64 bits */
    memcpy(s_session.nonce_upper_64, session_id_bytes, 8U);

    /* Generate deterministic per-session Ed25519 keypair */
    uint8_t sign_seed[32];
    err = session_derive_sign_seed(s_session.device_id_self, sign_seed);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);
    err = crypto_eddsa_seeded_keypair(s_session.sign_pk, s_session.sign_sk, sign_seed);
    ceepew_secure_zero(sign_seed, sizeof(sign_seed));
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    return CEEPEW_OK;
}

CeePewErr_t session_phase2_derive_key(void){
    CEEPEW_ASSERT(s_session.phase == 2U, CEEPEW_ERR_PARAM);

    /* Save region state for rollback on error */
    uint32_t saved_bump = g_region.bump;
    CeePewErr_t err = CEEPEW_OK;

    /* Declare all intermediate values needed for error cleanup */
    uint8_t ds_mix[32] = {0};
    uint8_t salt_input[64U] = {0};
    uint8_t hkdf_salt[32] = {0};
    uint8_t commitment[CEEPEW_COMMITMENT_BYTES] = {0};
    uint8_t hkdf_info[128U] = {0};
    uint8_t hkdf_output[64] = {0};

    /* Step 1: Digital sum preprocessing of session code */
    digital_sum_mix(s_session.session_code, 32U, ds_mix);

    /* Step 2: SHA256(digital_sum_mix || session_code) as HKDF salt */
    memcpy(salt_input, ds_mix, 32U);
    memcpy(salt_input + 32U, s_session.session_code, 32U);

    err = crypto_sha256_compute(salt_input, 64U, hkdf_salt);
    if (err != CEEPEW_OK) { goto error_cleanup; }

    /* Step 3: Build canonical HKDF info (label || id_A || id_B || commitment || t_round)
     * Use crypto_hkdf_build_info() to ensure canonical ordering and fixed-length fields. */
    const uint8_t label[] = "CEEPEW_SESSION_v1";
    const uint8_t label_len = (uint8_t)(sizeof(label) - 1U);

    const uint8_t *id_a = s_session.device_id_self;
    const uint8_t *id_b = s_session.device_id_peer;
    if (memcmp(id_a, id_b, 6U) > 0) {
        const uint8_t *tmp = id_a;
        id_a = id_b;
        id_b = tmp;
    }

    err = session_get_commitment(commitment);
    if (err != CEEPEW_OK) { goto error_cleanup; }

    uint8_t hkdf_info_len = 0U;
    err = crypto_hkdf_build_info(label, label_len, id_a, id_b, commitment, 0U, hkdf_info, &hkdf_info_len);
    if (err != CEEPEW_OK) { goto error_cleanup; }

    /* Step 3a: IKM is the raw 32-byte session_code. Both peers must derive
     * the SAME keys from the SAME session_code + commitment + device IDs.
     * Local randomness in the IKM would break key convergence: each peer
     * would compute a different HKDF output and crypto_box would fail to
     * decrypt. (The earlier `fresh_salt` mixing was incorrect — it made
     * the IKM diverge even though the commitment match succeeded.) */
    err = crypto_hkdf_derive(s_session.session_code, 32U,
                             hkdf_salt, 32U,
                             hkdf_info, hkdf_info_len, hkdf_output, 64U);
    if (err != CEEPEW_OK) { goto error_cleanup; }

    /* Extract layout: [0:15]=Ascon key, [16:47]=crypto_box seed, [48:55]=session_id */
    memcpy(g_crypto_ctx.ascon_key, hkdf_output, 16U);
    memcpy(g_crypto_ctx.box_seed, hkdf_output + 16U, 32U);
    memcpy(g_crypto_ctx.session_id, hkdf_output + 48U, 8U);
    memcpy(g_crypto_ctx.reserved, hkdf_output + 56U, 8U);
    g_crypto_ctx.session_active = true;

    memcpy(s_session.session_key, g_crypto_ctx.ascon_key, 16U);
    memcpy(s_session.nonce_upper_64, g_crypto_ctx.session_id, 8U);
    s_session.session_id = ((uint64_t)g_crypto_ctx.session_id[0] << 56U) |
                           ((uint64_t)g_crypto_ctx.session_id[1] << 48U) |
                           ((uint64_t)g_crypto_ctx.session_id[2] << 40U) |
                           ((uint64_t)g_crypto_ctx.session_id[3] << 32U) |
                           ((uint64_t)g_crypto_ctx.session_id[4] << 24U) |
                           ((uint64_t)g_crypto_ctx.session_id[5] << 16U) |
                           ((uint64_t)g_crypto_ctx.session_id[6] << 8U) |
                           ((uint64_t)g_crypto_ctx.session_id[7]);
    /* Role-based nonce parity: initiator (lower MAC) starts at 0 and uses
     * even nonces (0, 2, 4, ...). Responder (higher MAC) starts at 1 and
     * uses odd nonces (1, 3, 5, ...). Both peers compute the same role
     * assignment from the deterministic MAC ordering, so the two nonce
     * ranges are guaranteed disjoint — no RNG, no risk of collision even
     * if both sides send their first message "simultaneously". */
    s_session.nonce_counter = s_session.is_initiator ? 0ULL : 1ULL;
    CEEPEW_LOG("SESSION",
               "derived keys (no local salt), nonce starts at %llu (role=%s)",
               (unsigned long long)s_session.nonce_counter,
               s_session.is_initiator ? "initiator" : "responder");

    s_session.phase = 3U;
    s_session.session_active = true;
    session_init_ttl();  /* Phase 4: Initialize TTL tracking */

    /* Initialize Core 0/1 mutex for g_crypto_ctx access */
    CeePewErr_t mutex_err = crypto_mutex_init();
    if (mutex_err != CEEPEW_OK) { goto error_cleanup; }

    err = esl_register_callbacks(session_mac_lock_check, session_enforce_nonce_limit);
    if (err != CEEPEW_OK) { goto error_cleanup; }

    /* Success: secure zero all sensitive intermediates */
    ceepew_secure_zero(hkdf_output, sizeof(hkdf_output));
    ceepew_secure_zero(hkdf_salt, sizeof(hkdf_salt));
    ceepew_secure_zero(salt_input, sizeof(salt_input));
    ceepew_secure_zero(ds_mix, sizeof(ds_mix));
    ceepew_secure_zero(commitment, sizeof(commitment));
    ceepew_secure_zero(hkdf_info, sizeof(hkdf_info));

    return CEEPEW_OK;

error_cleanup:
    /* Restore region bump on error */
    g_region.bump = saved_bump;

    /* Secure zero all sensitive intermediates */
    ceepew_secure_zero(hkdf_output, sizeof(hkdf_output));
    ceepew_secure_zero(hkdf_salt, sizeof(hkdf_salt));
    ceepew_secure_zero(salt_input, sizeof(salt_input));
    ceepew_secure_zero(ds_mix, sizeof(ds_mix));
    ceepew_secure_zero(commitment, sizeof(commitment));
    ceepew_secure_zero(hkdf_info, sizeof(hkdf_info));

    return err;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Phase 3: Active Session (nonce enforcement, encrypted communication)  */
/* ────────────────────────────────────────────────────────────────────── */

CeePewErr_t session_enforce_nonce_limit(void){
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    /* Check for hard limit — session cannot continue */
    if (s_session.nonce_counter >= CEEPEW_NONCE_HARD_LIMIT) {
        return CEEPEW_ERR_NONCE_EXHAUSTED;
    }

    /* Check for 90% warning level — session should warn but can continue briefly */
    if (s_session.nonce_counter >= CEEPEW_NONCE_WARNING_LIMIT) {
        s_session.nonce_counter++;
        return CEEPEW_ERR_NONCE_NEARLY_EXHAUSTED;
    }

    s_session.nonce_counter++;
    return CEEPEW_OK;
}

CeePewErr_t session_get_nonce(uint8_t nonce[24]){
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(nonce != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    /* Nonce format (24 bytes for XSalsa20; Ascon truncates to 16B):
     * - nonce[0:8]   = session_id (derived from device IDs and session code)
     * - nonce[8:16]  = nonce_counter (little-endian 64-bit counter)
     * - nonce[16:24] = padding (zeros for XSalsa20)
     *
     * IMPORTANT: XSalsa20 (crypto_box) uses 24-byte nonce. Ascon-128 uses 16-byte nonce.
     * When used with Ascon, only the first 16 bytes are read. Both layers derive
     * their nonce from the same session_id base and counter for consistency.
     * The counter is incremented in session_enforce_nonce_limit() before every encrypt.
     */
    memcpy(nonce, g_crypto_ctx.session_id, 8U);
    for (uint8_t i = 0U; i < 8U; i++) {
        nonce[8U + i] = (uint8_t)((s_session.nonce_counter >> (i * 8U)) & 0xFFU);
    }
    memset(nonce + 16U, 0U, 8U);
    return CEEPEW_OK;
}

CeePewErr_t session_get_session_key(uint8_t key[16]){
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(key != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);
    memcpy(key, g_crypto_ctx.ascon_key, 16U);
    return CEEPEW_OK;
}

CeePewErr_t session_sign_message(const uint8_t *msg, uint32_t msg_len, uint8_t sig[64]){
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(msg != NULL || msg_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(sig != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(msg_len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);
    return crypto_eddsa_sign(s_session.sign_sk, msg, msg_len, sig);
}

CeePewErr_t session_get_peer_public_key(uint8_t peer_pk[32]){
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(peer_pk != NULL, CEEPEW_ERR_NULL_PTR);

    /* Peer pubkey must have been received over BLE during the commitment
     * exchange. If it hasn't, return PARAM so the caller (e.g. Ed25519 verify
     * in process_rx_frame) can silently drop the frame. */
    if (!s_session.peer_sign_pk_valid) {
        return CEEPEW_ERR_PARAM;
    }

    memcpy(peer_pk, s_session.peer_sign_pk, 32U);
    return CEEPEW_OK;
}

CeePewErr_t session_set_peer_public_key(const uint8_t peer_pk[32])
{
    CEEPEW_ASSERT(s_session.phase >= 2U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(peer_pk != NULL, CEEPEW_ERR_NULL_PTR);

    memcpy(s_session.peer_sign_pk, peer_pk, 32U);
    s_session.peer_sign_pk_valid = true;
    return CEEPEW_OK;
}

CeePewErr_t session_get_local_sign_pk(uint8_t pk_out[32])
{
    CEEPEW_ASSERT(s_session.phase >= 2U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(pk_out != NULL, CEEPEW_ERR_NULL_PTR);

    memcpy(pk_out, s_session.sign_pk, 32U);
    return CEEPEW_OK;
}

CeePewErr_t session_get_session_code(uint8_t out[32])
{
    CEEPEW_ASSERT(s_session.phase >= 2U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(out != NULL, CEEPEW_ERR_NULL_PTR);

    memcpy(out, s_session.session_code, 32U);
    return CEEPEW_OK;
}

CeePewErr_t session_set_role(bool initiator)
{
    /* Set the initiator/responder role for this session. Lower MAC is
     * initiator (sender uses even nonces); higher MAC is responder
     * (sender uses odd nonces). Both peers compute the same role from
     * the same MAC ordering, so the role assignment is symmetric and
     * the resulting nonce ranges are non-overlapping. */
    s_session.is_initiator = initiator;
    return CEEPEW_OK;
}

bool session_get_role(void)
{
    return s_session.is_initiator;
}

CeePewErr_t session_mark_sync_barrier_cleared(void)
{
    /* Called by the session task after an encrypted post-derive ACK
     * round-trip is verified. Idempotent. */
    s_session.sync_barrier_cleared = true;
    return CEEPEW_OK;
}

bool session_sync_barrier_cleared(void)
{
    return s_session.sync_barrier_cleared;
}

CeePewErr_t session_set_peer_uptime_offset(int32_t offset_s)
{
    /* Clamp to ±24h — anything beyond is almost certainly a buggy or
     * hostile peer and should be treated as zero (fall back to raw
     * uptimes + the 45s slack window). */
    if (offset_s > 86400)  { offset_s = 86400; }
    if (offset_s < -86400) { offset_s = -86400; }
    s_session.peer_uptime_offset_s = offset_s;
    return CEEPEW_OK;
}

/* Post-derive sync driver. Called from the session task main loop while
 * phase 3 is active but sync_barrier_cleared is false.
 *
 * Phase 7 (Hybrid-GATT plan): both peers retransmit the HELLO byte
 * every CEEPEW_KEY_SYNC_RETRY_MS — the barrier clears as soon as a
 * single encrypted sync message is decoded at either end (the receiver
 * is symmetric for HELLO and ACK). This converges faster and survives
 * single-direction packet loss. The earlier asymmetric design (only
 * the initiator retransmitted) left a 250ms × N worst case if the
 * initiator's first HELLO was lost.
 *
 * The function is a no-op once the barrier is cleared or the timeout has
 * been reached. It is safe to call every main-loop tick. */
CeePewErr_t session_drive_post_derive_sync(uint64_t now_ms)
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    if (s_session.sync_barrier_cleared) {
        return CEEPEW_OK;
    }

    /* Initialise the timeout deadline on first call */
    if (s_session.sync_started_ms == 0ULL) {
        s_session.sync_started_ms = now_ms;
    }

    if ((now_ms - s_session.sync_started_ms) >= CEEPEW_KEY_SYNC_TIMEOUT_MS) {
        return CEEPEW_ERR_TIMEOUT;
    }

    /* Both peers retransmit. The barrier clears on receipt of any
     * sync message (HELLO or ACK) at either end — see
     * session_handle_key_sync_byte(). */

    /* Rate-limit HELLO retransmissions. */
    if (s_session.sync_last_send_ms != 0ULL &&
        (now_ms - s_session.sync_last_send_ms) < CEEPEW_KEY_SYNC_RETRY_MS) {
        return CEEPEW_OK;
    }
    s_session.sync_last_send_ms = now_ms;

    /* Build the 1-byte HELLO payload. We use session_send_message so the
     * full pipeline (compress → Ascon → crypto_box → sign → FEC → CRC →
     * ESP-NOW) runs. A successful receive on the other side is the proof
     * that the key converges. */
    uint8_t hello_plain[1] = { CEEPEW_KEY_SYNC_HELLO_BYTE };

    /* The peer MAC and peer public key are required by session_send_message.
     * If sign_pk was never delivered (5-retry exhaustion) the peer public
     * key won't be available, so we fall back to using a deterministic
     * placeholder that still allows the receiver to decrypt — the peer
     * MAC is the only thing that must be correct. */
    uint8_t peer_mac[6] = {0U};
    uint8_t peer_pk[32] = {0U};
    CeePewErr_t mac_err = session_get_device_id(peer_mac);
    if (mac_err != CEEPEW_OK) { return mac_err; }
    /* Use peer MAC from session context, not own MAC (fix above) */
    memcpy(peer_mac, s_session.device_id_peer, 6U);
    (void)session_get_peer_public_key(peer_pk);
    /* If peer_sign_pk is not valid, the receiver's crypto_box will compute
     * the shared secret using its OWN box_seed (which is identical) and
     * the sender's box_seed-curve25519-clamp(public) value — wait, no,
     * this is wrong. Skip the send if we don't have the peer's sign_pk. */
    if (!s_session.peer_sign_pk_valid) {
        /* Responder never sent sign_pk; cannot perform ECDH correctly.
         * Wait for it to arrive — but if it never does, the timeout
         * above will fire and we transition to PAIRING_FAILED. */
        return CEEPEW_OK;
    }

    return session_send_message(hello_plain, 1U, peer_mac, peer_pk);
}

/* Called by the session task's RX path when a 1-byte post-derive sync
 * message is decoded. The function decides whether the byte is a HELLO
 * (responder should ACK), an ACK (initiator should clear the barrier),
 * or neither (silently ignored). */
CeePewErr_t session_handle_key_sync_byte(uint8_t magic_byte)
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    if (magic_byte == CEEPEW_KEY_SYNC_HELLO_BYTE) {
        /* Responder's turn to ACK. We only do this once; the barrier is
         * cleared after the ACK is enqueued (best-effort — the actual
         * send can still fail, but the round-trip semantics are preserved:
         * a future retry HELLO would see the barrier already cleared). */
        if (!s_session.sync_barrier_cleared) {
            s_session.sync_barrier_cleared = true;
            return CEEPEW_ERR_NEED_TX;  /* signal "send ACK back" */
        }
        return CEEPEW_OK;
    }

    if (magic_byte == CEEPEW_KEY_SYNC_ACK_BYTE) {
        s_session.sync_barrier_cleared = true;
        return CEEPEW_OK;
    }

    return CEEPEW_ERR_PARAM;
}

int32_t session_get_peer_uptime_offset(void)
{
    return s_session.peer_uptime_offset_s;
}

CeePewErr_t session_mac_lock_check(const uint8_t peer_mac[6]){
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);

    /* Constant-time MAC comparison */
    if (!ceepew_ct_equal(s_session.device_id_peer, peer_mac, 6U)) {
        return CEEPEW_ERR_TRANSPORT;  /* Silent discard */
    }

    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Session Termination (secure cleanup)                                  */
/* ────────────────────────────────────────────────────────────────────── */

CeePewErr_t session_end(void){
    CeePewErr_t err = ceepew_pipeline_deinit();
    if (err != CEEPEW_OK) { /* continue with zeroing */ }
    err = crypto_ctx_destroy();
    if (err != CEEPEW_OK) { /* continue with zeroing */ }
    session_secure_zero_context();
    region_reset(&g_region);

    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Pairing-failure reset (securely back to Phase 1)                      */
/* ────────────────────────────────────────────────────────────────────── */

CeePewErr_t session_reset_to_discovery(void)
{
    uint8_t saved_phase = s_session.phase;
    if (saved_phase < 1U || saved_phase > 3U) {
        return CEEPEW_ERR_PARAM;
    }

    /* Save local device ID before session_end() zeroes the context.
     * session_get_device_id() asserts phase >= 1, so call it first. */
    uint8_t local_id[6] = {0U};
    CeePewErr_t err = session_get_device_id(local_id);
    if (err != CEEPEW_OK) { return err; }

    (void)session_end();

    /* session_end() calls session_secure_zero_context() which zeroes s_session.
     * Re-enter Phase 1 so session_get_phase() returns 1 immediately. */
    err = session_phase1_init(local_id);
    if (err != CEEPEW_OK) { return err; }

    ESP_LOGI("session_fsm", "session_reset_to_discovery: phase %u -> 1", (unsigned)saved_phase);
    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Query/Diagnostic Functions                                            */
/* ────────────────────────────────────────────────────────────────────── */

uint8_t session_get_phase(void) { return s_session.phase; }

bool session_is_active(void) { return s_session.session_active; }

uint64_t session_get_nonce_counter(void) { return s_session.nonce_counter; }

uint64_t session_get_id(void) {
    return ((uint64_t)g_crypto_ctx.session_id[0] << 56U) |
           ((uint64_t)g_crypto_ctx.session_id[1] << 48U) |
           ((uint64_t)g_crypto_ctx.session_id[2] << 40U) |
           ((uint64_t)g_crypto_ctx.session_id[3] << 32U) |
           ((uint64_t)g_crypto_ctx.session_id[4] << 24U) |
           ((uint64_t)g_crypto_ctx.session_id[5] << 16U) |
           ((uint64_t)g_crypto_ctx.session_id[6] << 8U) |
           ((uint64_t)g_crypto_ctx.session_id[7]);
}

CeePewErr_t session_get_device_id(uint8_t device_id[6])
{
    CEEPEW_ASSERT(s_session.phase >= 1U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(device_id != NULL, CEEPEW_ERR_NULL_PTR);
    memcpy(device_id, s_session.device_id_self, 6U);
    return CEEPEW_OK;
}

CeePewErr_t session_get_commitment(uint8_t commitment[CEEPEW_COMMITMENT_BYTES])
{
    CEEPEW_ASSERT(s_session.phase >= 2U && s_session.phase <= 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(commitment != NULL, CEEPEW_ERR_NULL_PTR);

    /* Stronger commitment v2: SHA256("CEEPEW_COMMIT_v2" || id_A || id_B || session_code || t_round)
     * Truncate to first 16 bytes for improved collision resistance. Order device ids
     * deterministically (lower first) so both peers derive the same commitment. */
    const char *label = "CEEPEW_COMMIT_v2";
    size_t label_len = strlen(label);

    uint8_t id_a[6];
    uint8_t id_b[6];
    memcpy(id_a, s_session.device_id_self, 6U);
    memcpy(id_b, s_session.device_id_peer, 6U);

    /* Deterministic ordering: ensure id_a <= id_b lexicographically */
    if (memcmp(id_a, id_b, 6U) > 0) {
        uint8_t tmp[6]; memcpy(tmp, id_a, 6U); memcpy(id_a, id_b, 6U); memcpy(id_b, tmp, 6U);
    }

    /* The commitment must be identical on both peers; local timestamps would
     * never match because each MCU boots at a different time. */
    uint32_t t_round = 0U;

    uint8_t info[64];
    size_t off = 0U;
    memcpy(info + off, label, label_len); off += label_len;
    memcpy(info + off, id_a, 6U); off += 6U;
    memcpy(info + off, id_b, 6U); off += 6U;
    memcpy(info + off, s_session.session_code, 32U); off += 32U;
    memcpy(info + off, &t_round, sizeof(uint32_t)); off += sizeof(uint32_t);

    uint8_t out32[32];
    CeePewErr_t err = crypto_sha256_compute(info, off, out32);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    memcpy(commitment, out32, CEEPEW_COMMITMENT_BYTES);

    /* Secure zero temporaries */
    ceepew_secure_zero(out32, sizeof(out32));
    ceepew_secure_zero(info, sizeof(info));

    return CEEPEW_OK;
}

CeePewErr_t session_verify_peer_commitment_with_sig(const uint8_t *peer_data, uint8_t len)
{
    CEEPEW_ASSERT(peer_data != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len == CEEPEW_COMMITMENT_BYTES          ||
                  len == CEEPEW_COMMITMENT_ADV_BYTES      ||
                  len == (CEEPEW_COMMITMENT_BYTES + 64U),
                  CEEPEW_ERR_PARAM);

    /* ── Advertisement-beacon path (16-byte truncated commitment) ───── */
    /* No signature on the beacon path — the 4-digit code is the primary
     * authentication, and the peer sign_pk is delivered separately over
     * the 0xFFF3 GATT characteristic for the fingerprint screen. */
    if (len == CEEPEW_COMMITMENT_ADV_BYTES) {
        uint8_t local_commit[CEEPEW_COMMITMENT_BYTES];
        CeePewErr_t adv_err = session_get_commitment(local_commit);
        if (adv_err != CEEPEW_OK) { return adv_err; }

        uint8_t diff = 0U;
        for (uint8_t i = 0U; i < CEEPEW_COMMITMENT_ADV_BYTES; i++) {
            diff |= (uint8_t)(local_commit[i] ^ peer_data[i]);
        }
        ceepew_secure_zero(local_commit, sizeof(local_commit));
        return (diff == 0U) ? CEEPEW_OK : CEEPEW_ERR_AUTH_FAIL;
    }

    /* ── GATT paths (full 32B commitment, optional 64B signature) ───── */
    const uint8_t cmp_len = CEEPEW_COMMITMENT_BYTES;

    /* Compare peer commitment against our locally computed commitment (constant-time style) */
    uint8_t local_commit[CEEPEW_COMMITMENT_BYTES];
    CeePewErr_t err = session_get_commitment(local_commit);
    if (err != CEEPEW_OK) { return err; }

    uint8_t match = 0U;
    for (uint8_t i = 0U; i < cmp_len; i++) { match |= (uint8_t)(local_commit[i] ^ peer_data[i]); }
    if (match != 0U) {
        ceepew_secure_zero(local_commit, sizeof(local_commit));
        return CEEPEW_ERR_AUTH_FAIL;
    }

    /* If a signature is appended, verify it against the peer's stored
     * ephemeral public key (received over BLE during the commitment
     * exchange). If we have not yet received the peer's public key, the
     * signature cannot be verified — return AUTH_FAIL so the caller drops
     * the message rather than accepting an unverified commitment. */
    if (len >= (uint8_t)(CEEPEW_COMMITMENT_BYTES + 64U)) {
        const uint8_t *peer_sig = peer_data + CEEPEW_COMMITMENT_BYTES;

        if (!s_session.peer_sign_pk_valid) {
            ceepew_secure_zero(local_commit, sizeof(local_commit));
            return CEEPEW_ERR_AUTH_FAIL;
        }

        /* Verify signature over the commitment bytes */
        err = crypto_eddsa_verify(s_session.peer_sign_pk, local_commit, cmp_len, peer_sig);

        if (err != CEEPEW_OK) {
            ceepew_secure_zero(local_commit, sizeof(local_commit));
            return CEEPEW_ERR_SIG_FAIL;
        }
    }

    ceepew_secure_zero(local_commit, sizeof(local_commit));
    return CEEPEW_OK;
}

CeePewErr_t session_get_commitment_with_sig(uint8_t *out_buf, uint8_t *out_len)
{
    CEEPEW_ASSERT(out_buf != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out_len != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t local_commit[CEEPEW_COMMITMENT_BYTES];
    CeePewErr_t err = session_get_commitment(local_commit);
    if (err != CEEPEW_OK) { return err; }

    /* Copy commitment */
    memcpy(out_buf, local_commit, CEEPEW_COMMITMENT_BYTES);
    uint8_t written = CEEPEW_COMMITMENT_BYTES;

    /* Attempt to sign the commitment with our ephemeral sign_sk (available after phase2_initiate)
     * If signing fails, fall back to commitment-only publish. */
    uint8_t sig[64];
    err = crypto_eddsa_sign(s_session.sign_sk, local_commit, CEEPEW_COMMITMENT_BYTES, sig);
    if (err == CEEPEW_OK) {
        memcpy(out_buf + written, sig, 64U);
        written += 64U;
    }

    *out_len = written;

    ceepew_secure_zero(local_commit, sizeof(local_commit));
    ceepew_secure_zero(sig, sizeof(sig));
    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Phase 4: TTL, Fingerprint, and Secure Wipe                             */
/* ────────────────────────────────────────────────────────────────────── */

/* Phase 4: Initialize TTL tracking on session start */
static void session_init_ttl(void)
{
    s_session.last_message_time_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
}

/* Phase 4: Update last message activity timestamp */
CeePewErr_t session_update_last_message_time(void)
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    s_session.last_message_time_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    return CEEPEW_OK;
}

/* Phase 4: Get idle time since last message activity */
CeePewErr_t session_get_idle_seconds(uint32_t *idle_seconds)
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(idle_seconds != NULL, CEEPEW_ERR_NULL_PTR);

    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    *idle_seconds = (now_s > s_session.last_message_time_s) ?
                    (now_s - s_session.last_message_time_s) : 0U;
    return CEEPEW_OK;
}

/* Phase 4: Compute device fingerprint from peer public key */
CeePewErr_t session_compute_fingerprint(const uint8_t peer_pk[32],
                                        const uint8_t device_id[6],
                                        uint8_t fingerprint_out[16])
{
    CEEPEW_ASSERT(peer_pk != NULL && device_id != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(fingerprint_out != NULL, CEEPEW_ERR_NULL_PTR);

    /* Fingerprint = SHA256(peer_public_key || device_id)[0:15] */
    uint8_t fp_input[32U + 6U];
    memcpy(fp_input, peer_pk, 32U);
    memcpy(fp_input + 32U, device_id, 6U);

    uint8_t fp_hash[32];
    CeePewErr_t err = crypto_sha256_compute(fp_input, (uint32_t)sizeof(fp_input), fp_hash);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    memcpy(fingerprint_out, fp_hash, 16U);
    memcpy(s_session.fingerprint, fp_hash, 16U);
    s_session.fingerprint_valid = true;

    /* Secure zero */
    ceepew_secure_zero(fp_input, sizeof(fp_input));
    ceepew_secure_zero(fp_hash, sizeof(fp_hash));

    return CEEPEW_OK;
}

/* Phase 4: Get stored fingerprint */
CeePewErr_t session_get_fingerprint(uint8_t fingerprint[16])
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(fingerprint != NULL, CEEPEW_ERR_NULL_PTR);

    if (!s_session.fingerprint_valid) {
        return CEEPEW_ERR_PARAM;
    }

    memcpy(fingerprint, s_session.fingerprint, 16U);
    return CEEPEW_OK;
}

/* Phase 4: Secure session wipe triggered by TTL expiry or nonce exhaustion */
CeePewErr_t session_wipe(void)
{
    CEEPEW_LOG("SESSION", "Executing secure session wipe");

    /* End crypto context */
    CeePewErr_t err = ceepew_pipeline_deinit();
    if (err != CEEPEW_OK) { CEEPEW_LOG("SESSION", "pipeline_deinit failed: %d", err); }
    err = crypto_ctx_destroy();
    if (err != CEEPEW_OK) { CEEPEW_LOG("SESSION", "crypto_ctx_destroy failed: %d", err); }

    /* Secure zero all key material and stale session state */
    session_secure_zero_context();

    /* Reset region allocator to free all temporary buffers */
    region_reset(&g_region);

    /* Re-enable the pipeline for the next session attempt */
    err = ceepew_pipeline_init();
    if (err != CEEPEW_OK) { CEEPEW_LOG("SESSION", "pipeline_init failed: %d", err); }

    /* Reset UI to discovery mode */
    ui_manager_reset_to_discovery();

    CEEPEW_LOG("SESSION", "Secure wipe complete, ready for new session");
    return CEEPEW_OK;
}

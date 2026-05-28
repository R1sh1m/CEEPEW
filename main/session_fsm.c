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
 * - Key derivation: HKDF-SHA256(SHA256(digital_sum_mix(code) || code), info)
 *   where info binds device IDs, session commitment, and timestamp
 * - Nonce enforcement: Counter must be < CEEPEW_NONCE_HARD_LIMIT; incremented AFTER check
 * - Replay defense: 64-bit WireGuard bitmap + ±15s timestamp window
 * - Signing: Ed25519 per-session ephemeral keypairs (destroyed on session_end)
 * - Secure zeroing: All key material (session_key, nonce_counter, sign_sk) volatile-written
 */

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "ceepew_security_utils.h"
#include "crypto_ctx.h"
#include "transport_esl.h"
#include "ceepew_pipeline.h"
#include "ceepew_region.h"  /* Phase 4: For region_reset */
#include "crypto_eddsa.h"
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
    uint64_t session_id;                         /* Derived from timestamp + nonce base */
    uint8_t  device_id_self[6];                  /* Own MAC */
    uint8_t  device_id_peer[6];                  /* Peer MAC */
    uint8_t  session_code[32];                   /* Human-verified pairing code */
    uint8_t  session_key[16];                    /* Ascon-128 key (Phase 3 only) */
    uint64_t nonce_counter;                      /* Must be < CEEPEW_NONCE_HARD_LIMIT */
    uint8_t  nonce_upper_64[8];                  /* session_id */
    uint8_t  sign_pk[32];                        /* Ephemeral Ed25519 public key */
    uint8_t  sign_sk[64];                        /* VOLATILE ephemeral private key */
    uint32_t phase2_timestamp;                   /* Phase 2 initiation time */
    bool     session_active;                     /* true = Phase 3 && authenticated */
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

static CeePewErr_t session_derive_sign_seed(const uint8_t mac[6], uint8_t seed_out[32])
{
    CEEPEW_ASSERT(mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(seed_out != NULL, CEEPEW_ERR_NULL_PTR);
    uint8_t seed_material[32U + 6U];
    memcpy(seed_material, s_session.session_code, 32U);
    memcpy(seed_material + 32U, mac, 6U);
    CeePewErr_t err = crypto_sha256_compute(seed_material, (uint32_t)sizeof(seed_material), seed_out);
    ceepew_secure_zero(seed_material, (uint32_t)sizeof(seed_material));
    return err;
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

    s_session.phase = 2U;
    memcpy(s_session.session_code, session_code, 32U);
    s_session.phase2_timestamp = (uint32_t)(esp_timer_get_time() / 1000000LL);

    /* Derive session_id from timestamp + peer MAC */
    uint8_t session_id_src[16U];
    memcpy(session_id_src, s_session.device_id_peer, 6U);
    memcpy(session_id_src + 6U, &s_session.phase2_timestamp, 4U);
    memcpy(session_id_src + 10U, s_session.device_id_self, 6U);

    uint8_t session_id_hash[32];
    CeePewErr_t err = crypto_sha256_compute(session_id_src, 14U, session_id_hash);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    s_session.session_id = ((uint64_t)session_id_hash[0] << 56U) |
                           ((uint64_t)session_id_hash[1] << 48U) |
                           ((uint64_t)session_id_hash[2] << 40U) |
                           ((uint64_t)session_id_hash[3] << 32U) |
                           ((uint64_t)session_id_hash[4] << 24U) |
                           ((uint64_t)session_id_hash[5] << 16U) |
                           ((uint64_t)session_id_hash[6] << 8U) |
                           ((uint64_t)session_id_hash[7]);

    /* Store session_id in nonce upper 64 bits */
    for (uint8_t i = 0U; i < 8U; i++) {
        s_session.nonce_upper_64[i] = session_id_hash[i];
    }

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

    /* Step 1: Digital sum preprocessing of session code */
    uint8_t ds_mix[32];
    digital_sum_mix(s_session.session_code, 32U, ds_mix);

    /* Step 2: SHA256(digital_sum_mix || session_code) as HKDF salt */
    uint8_t salt_input[64U];
    memcpy(salt_input, ds_mix, 32U);
    memcpy(salt_input + 32U, s_session.session_code, 32U);

    uint8_t hkdf_salt[32];
    CeePewErr_t err = crypto_sha256_compute(salt_input, 64U, hkdf_salt);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    /* Step 3: Build HKDF info (bind all session parameters) */
    /* "CEEPEW_SESSION_v1" || id_A[6] || id_B[6] || commitment[32] || t_round[4] */
    const char *label = "CEEPEW_SESSION_v1";
    const size_t label_len = 17U;

    uint8_t hkdf_info[65U];
    size_t info_off = 0U;
    memcpy(hkdf_info + info_off, label, label_len);
    info_off += label_len;

    const uint8_t *id_a = s_session.device_id_self;
    const uint8_t *id_b = s_session.device_id_peer;
    if (memcmp(id_a, id_b, 6U) > 0) {
        const uint8_t *tmp = id_a;
        id_a = id_b;
        id_b = tmp;
    }

    memcpy(hkdf_info + info_off, id_a, 6U);
    info_off += 6U;

    memcpy(hkdf_info + info_off, id_b, 6U);
    info_off += 6U;

    memcpy(hkdf_info + info_off, s_session.session_code, 32U);
    info_off += 32U;

    /* All devices must hash the exact same info bytes. A device-local boot
     * timestamp would diverge here, so t_round is intentionally fixed. */
    uint32_t t_round = 0U;
    memcpy(hkdf_info + info_off, &t_round, 4U);
    info_off += 4U;

    CEEPEW_ASSERT(info_off == 65U, CEEPEW_ERR_BOUNDS);

    /* Step 4-5: HKDF Derive (combined extract + expand, 64 bytes output) */
    uint8_t hkdf_output[64];
    err = crypto_hkdf_derive(s_session.session_code, 32U, hkdf_salt, 32U, hkdf_info, (uint8_t)info_off, hkdf_output, 64U);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    /* Extract layout: [0:15]=Ascon key, [16:47]=crypto_box seed, [48:55]=session_id */
    memcpy(g_crypto_ctx.ascon_key, hkdf_output, 16U);
    memcpy(g_crypto_ctx.box_seed, hkdf_output + 16U, 32U);
    memcpy(g_crypto_ctx.session_id, hkdf_output + 48U, 8U);
    memcpy(g_crypto_ctx.reserved, hkdf_output + 56U, 8U);
    g_crypto_ctx.session_active = true;
    g_crypto_ctx.nonce_counter = 0ULL;

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
    s_session.nonce_counter = 0ULL;

    /* Secure zero the intermediate values */
    ceepew_secure_zero(hkdf_output, sizeof(hkdf_output));
    ceepew_secure_zero(hkdf_salt, sizeof(hkdf_salt));
    ceepew_secure_zero(salt_input, sizeof(salt_input));
    ceepew_secure_zero(ds_mix, sizeof(ds_mix));

    s_session.phase = 3U;
    s_session.session_active = true;
    session_init_ttl();  /* Phase 4: Initialize TTL tracking */

    err = esl_register_callbacks(session_mac_lock_check, session_enforce_nonce_limit);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Phase 3: Active Session (nonce enforcement, encrypted communication)  */
/* ────────────────────────────────────────────────────────────────────── */

CeePewErr_t session_enforce_nonce_limit(void){
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    /* Before every encrypt operation, check nonce hasn't exhausted */
    if (s_session.nonce_counter >= CEEPEW_NONCE_HARD_LIMIT) { return CEEPEW_ERR_NONCE_EXHAUSTED; }

    s_session.nonce_counter++;
    g_crypto_ctx.nonce_counter = s_session.nonce_counter;
    return CEEPEW_OK;
}

CeePewErr_t session_get_nonce(uint8_t nonce[24]){
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(nonce != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    /* nonce[0:8] = session_id, nonce[8:16] = nonce_counter (LE), nonce[16:24] = padding */
    memcpy(nonce, g_crypto_ctx.session_id, 8U);
    for (uint8_t i = 0U; i < 8U; i++) {
        nonce[8U + i] = (uint8_t)((g_crypto_ctx.nonce_counter >> (i * 8U)) & 0xFFU);
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

    uint8_t peer_seed[32];
    CeePewErr_t err = session_derive_sign_seed(s_session.device_id_peer, peer_seed);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);
    uint8_t peer_sk[64];
    err = crypto_eddsa_seeded_keypair(peer_pk, peer_sk, peer_seed);
    ceepew_secure_zero(peer_sk, sizeof(peer_sk));
    ceepew_secure_zero(peer_seed, sizeof(peer_seed));
    CEEPEW_ASSERT(err == CEEPEW_OK, err);
    return CEEPEW_OK;
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
/* Query/Diagnostic Functions                                            */
/* ────────────────────────────────────────────────────────────────────── */

uint8_t session_get_phase(void) { return s_session.phase; }

bool session_is_active(void) { return s_session.session_active; }

uint64_t session_get_nonce_counter(void) { return g_crypto_ctx.nonce_counter; }

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

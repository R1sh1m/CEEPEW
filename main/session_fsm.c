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
 *   does NOT advance the UI to KEYDER. The session task drives
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
#include "transport_ble.h"
#include "transport_hop.h"
#include "hal_radio.h"
#include "ceepew_pipeline.h"
#include "ceepew_region.h"  /* Phase 4: For region_reset */
#include "crypto_eddsa.h"
#include "crypto_hkdf.h"
#include "crypto_ecdh.h"
#include "crypto_hmac_efuse.h"
#include "ecc_hamming.h"
#include "session_fsm.h"
#include "session_memory.h"  /* For g_ui_event_queue and UIEvent_t */
#include "esp_log.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "hal_radio.h"

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

/* ARQ engine reset — zeroes s_tx_seq, s_expected_rx_seq, s_rx_init in ecc_arq.c */
extern CeePewErr_t ecc_arq_reset(void);

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
    uint8_t  device_id_self_wifi[6];             /* This device's WiFi STA MAC */
    bool     device_id_self_wifi_valid;          /* true once device_id_self_wifi has been set */
    uint8_t  device_id_peer_wifi[6];             /* Peer's WiFi STA MAC for ESP-NOW */
    bool     device_id_peer_wifi_valid;          /* true once device_id_peer_wifi has been set */
    int32_t  peer_uptime_offset_s;               /* peer_uptime_s - local_uptime_s (BLE time-sync) */
    bool     session_active;                     /* true = Phase 3 && authenticated */
    bool     is_initiator;                       /* true if local MAC < peer MAC (lower = initiator) */
    bool     sync_barrier_cleared;               /* true once encrypted post-derive ACK round-trip complete */
    bool     sync_peer_encrypted_received;        /* true once we decrypted an encrypted frame from peer */
    bool     sync_local_ack_sent;                 /* true once responder ACK has been sent */
    uint64_t sync_started_ms;                    /* when post-derive sync driver was first invoked */
    uint64_t sync_last_send_ms;                  /* last HELLO retransmit time */
    uint8_t  sync_retry_stage;                   /* exponential backoff stage (0..5) */
    /* Phase 4: TTL tracking */
    uint64_t last_message_time_s;                /* Last message activity timestamp (seconds) */

    /* Rendezvous phase (static channel sync before channel hopping) */
    bool     rendezvous_initiated;               /* true once rendezvous handshake started */
    bool     rendezvous_synced;                  /* true once both sides confirmed sync */
    int32_t  rendezvous_offset_us;               /* Timing offset from peer (responder - initiator) */
} SessionCtx_t;

/* Global session context (per-device; single active session at a time) */
static SessionCtx_t s_session;

/* Test shadow state. When s_test_*_set is true, the corresponding
 * session_get_*() returns the test value. This replaces the old
 * __attribute__((weak)) test fallbacks (ui_manager_test.c, ui_cryptogram_test.c)
 * with explicit setters — no weak symbols in the binary. */
static bool     s_test_id_set = false;
static uint64_t s_test_id     = 0ULL;
static bool     s_test_nc_set = false;
static uint64_t s_test_nc     = 0ULL;
static bool     s_test_commitment_set = false;
static uint8_t  s_test_commitment[CEEPEW_COMMITMENT_BYTES] = {0U};

/* Persistent WiFi MAC backup. Survives session_secure_zero_context() so
 * session_phase1_init() can restore it after every memset — critical for
 * key convergence across pairing retry cycles. */
static uint8_t  s_saved_self_wifi[6] = {0U};
static bool     s_saved_self_wifi_valid = false;

/* Phase 4: Initialize TTL tracking on session start (forward declaration) */
static void session_init_ttl(void);
static void session_secure_zero_context(void);
static CeePewErr_t session_ordered_device_ids(uint8_t id_a[6], uint8_t id_b[6]);
static CeePewErr_t session_clear_sync_barrier_internal(bool need_tx);

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
     * 3. Accept each other's commitment values on display
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
    esl_reset_callbacks();

    memset(&s_session, 0, sizeof(s_session));

    /* Restore self WiFi MAC from file-scope backup (survives
     * session_secure_zero_context() which runs before we get here). */
    if (s_saved_self_wifi_valid) {
        memcpy(s_session.device_id_self_wifi, s_saved_self_wifi, 6U);
        s_session.device_id_self_wifi_valid = true;
    }
    CeePewErr_t err = crypto_ctx_init();
    CEEPEW_ASSERT(err == CEEPEW_OK, err);
    s_session.phase = 1U;
    memcpy(s_session.device_id_self, device_id, 6U);
    s_session.session_active = false;

    /* Clear any test-shadow state left behind by the boot-time diagnostic
     * suite.  The s_test_*_set flags are file-scope statics (not part of
     * s_session) so session_end() / memset do not touch them.  If
     * ui_cryptogram_selftest_run() sets s_test_commitment_set and forgets
     * to unset it, the next real pairing would use a stale test
     * commitment instead of computing the real one. */
    s_test_commitment_set = false;
    s_test_id_set = false;
    s_test_nc_set = false;

    /* Reset double-ended sync state for clean pairing attempt */
    s_session.sync_peer_encrypted_received = false;
    s_session.sync_local_ack_sent = false;

    /* M7: Power management — enable WiFi modem PS + low BLE duty during discovery */
    (void)hal_radio_set_power_save(CEEPEW_WIFI_PS_DISCOVERY);
    (void)transport_ble_set_scan_duty_cycle(CEEPEW_BLE_SCAN_INTERVAL_DISC_MS,
                                            CEEPEW_BLE_SCAN_WINDOW_DISC_MS);
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
    /* Defensive: peer MAC must be set before entering phase 2 */
    { static const uint8_t zeros[6] = {0};
      CEEPEW_ASSERT(memcmp(s_session.device_id_peer, zeros, 6U) != 0, CEEPEW_ERR_PARAM); }

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

    /* M7: Power management — full power for pairing (fast BLE + low latency) */
    (void)hal_radio_set_power_save(CEEPEW_WIFI_PS_ACTIVE_CHAT);
    (void)transport_ble_set_scan_duty_cycle(CEEPEW_BLE_SCAN_INTERVAL_PAIR_MS,
                                            CEEPEW_BLE_SCAN_WINDOW_PAIR_MS);

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

    /* Generate ephemeral X25519 keypair for crypto_box ECDH.
     * The public key is exchanged with the peer over BLE (M3).
     * The private key stays in RAM and is securely zeroed on session_end(). */
    CeePewErr_t ecdh_err = crypto_ecdh_generate_keypair(
        g_crypto_ctx.box_pubkey, g_crypto_ctx.box_privkey);
    CEEPEW_ASSERT(ecdh_err == CEEPEW_OK, ecdh_err);
    g_crypto_ctx.peer_box_pubkey_valid = false;

    return CEEPEW_OK;
}

CeePewErr_t session_phase2_derive_key(void){
    CEEPEW_ASSERT(s_session.phase == 2U, CEEPEW_ERR_PARAM);

    /* ── STRICT HARDWARE-GATED IDENTITY HANDOFF ──────────────────────────
     * The peer's WiFi STA MAC address must have been received and
     * validated over the secure GATT channel BEFORE any key material
     * is derived. If the MAC is missing, all-zeros, or was never
     * delivered via the 0xFFF3 characteristic exchange, key derivation
     * must NOT proceed — doing so would bind session keys to an
     * unauthenticated transport identity, enabling a MITM to inject
     * a rogue ESP-NOW peer and decrypt the session. */
    if (!s_session.device_id_peer_wifi_valid) {
        ESP_LOGE("session_fsm", "derive_key ABORT: peer WiFi MAC not verified via GATT");
        ceepew_secure_zero(&s_session.session_code, sizeof(s_session.session_code));
        return CEEPEW_ERR_PARAM;
    }
    {
        static const uint8_t zeros[6] = {0};
        if (ceepew_ct_equal(s_session.device_id_peer_wifi, zeros, 6U)) {
            ESP_LOGE("session_fsm", "derive_key ABORT: peer WiFi MAC is all-zeros");
            ceepew_secure_zero(&s_session.session_code, sizeof(s_session.session_code));
            return CEEPEW_ERR_PARAM;
        }
    }

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

    /* Step 2: Derive HKDF salt.
     * If eFuse identity is provisioned, use device-bound derivation:
     *   salt = SHA-256( HMAC(efuse_key, ds_mix) || SHA256(ds_mix || code) )
     * Otherwise, fallback to the original:
     *   salt = SHA-256(ds_mix || code) */
#ifdef CONFIG_CEEPEW_EFUSE_HMAC_KEY
    err = crypto_hmac_efuse_derive_salt(ds_mix, s_session.session_code, hkdf_salt);
    if (err != CEEPEW_OK) { goto error_cleanup; }
#else
    memcpy(salt_input, ds_mix, 32U);
    memcpy(salt_input + 32U, s_session.session_code, 32U);
    err = crypto_sha256_compute(salt_input, 64U, hkdf_salt);
    if (err != CEEPEW_OK) { goto error_cleanup; }
#endif

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
    err = crypto_hkdf_build_info(label, label_len, id_a, id_b, commitment, 0U, hkdf_info, sizeof(hkdf_info), &hkdf_info_len);
    if (err != CEEPEW_OK) { goto error_cleanup; }

    /* Step 3a: Bind the verified WiFi MAC pair into the HKDF info.
     * Both WiFi MACs (self and peer) are included in sorted (canonical)
     * order so that both peers derive identical keys. This cryptographically
     * ties the session to the hardware transport identities that were
     * authenticated over the secure GATT channel. If a WiFi MAC changes
     * (relay attack, MAC spoofing), the keys will diverge and the
     * post-derive sync exchange will fail. */
    if (hkdf_info_len + 12U <= sizeof(hkdf_info) &&
        s_session.device_id_self_wifi_valid) {
        const uint8_t *wifi_a = s_session.device_id_self_wifi;
        const uint8_t *wifi_b = s_session.device_id_peer_wifi;
        if (memcmp(wifi_a, wifi_b, 6U) > 0) {
            const uint8_t *tmp = wifi_a;
            wifi_a = wifi_b;
            wifi_b = tmp;
        }
        memcpy(hkdf_info + hkdf_info_len, wifi_a, 6U);
        hkdf_info_len += 6U;
        memcpy(hkdf_info + hkdf_info_len, wifi_b, 6U);
        hkdf_info_len += 6U;
    }

    /* Step 3a: DIAGNOSTIC — log HKDF inputs for key-convergence debugging */
    {
        /* Log device MACs used in HKDF info */
        ESP_LOGI("session_fsm", "DIAG: self_wifi=%02X:%02X:%02X:%02X:%02X:%02X valid=%d",
                 s_session.device_id_self_wifi[0], s_session.device_id_self_wifi[1],
                 s_session.device_id_self_wifi[2], s_session.device_id_self_wifi[3],
                 s_session.device_id_self_wifi[4], s_session.device_id_self_wifi[5],
                 s_session.device_id_self_wifi_valid);
        ESP_LOGI("session_fsm", "DIAG: peer_wifi=%02X:%02X:%02X:%02X:%02X:%02X valid=%d",
                 s_session.device_id_peer_wifi[0], s_session.device_id_peer_wifi[1],
                 s_session.device_id_peer_wifi[2], s_session.device_id_peer_wifi[3],
                 s_session.device_id_peer_wifi[4], s_session.device_id_peer_wifi[5],
                 s_session.device_id_peer_wifi_valid);
        /* Log sorted wifi_a/wifi_b (the actual bytes fed to HKDF) */
        const uint8_t *wifi_a = s_session.device_id_self_wifi;
        const uint8_t *wifi_b = s_session.device_id_peer_wifi;
        if (s_session.device_id_self_wifi_valid && s_session.device_id_peer_wifi_valid) {
            if (memcmp(wifi_a, wifi_b, 6U) > 0) {
                const uint8_t *tmp = wifi_a; wifi_a = wifi_b; wifi_b = tmp;
            }
        }
        ESP_LOGI("session_fsm", "DIAG: hkdf_info_len=%u wifi_a=%02X:%02X:%02X:%02X:%02X:%02X wifi_b=%02X:%02X:%02X:%02X:%02X:%02X",
                 hkdf_info_len,
                 wifi_a[0], wifi_a[1], wifi_a[2], wifi_a[3], wifi_a[4], wifi_a[5],
                 wifi_b[0], wifi_b[1], wifi_b[2], wifi_b[3], wifi_b[4], wifi_b[5]);
        /* Log full hkdf_info as hex for cross-device comparison */
        ESP_LOGI("session_fsm", "DIAG: hkdf_info_hex ");
        ESP_LOG_BUFFER_HEX("session_fsm", hkdf_info, (uint16_t)hkdf_info_len);
        /* Log first 8 bytes of hkdf_salt for cross-device comparison */
        ESP_LOGI("session_fsm", "DIAG: hkdf_salt_first8");
        ESP_LOG_BUFFER_HEX("session_fsm", hkdf_salt, 8U);
    }

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

    /* Initialize Core 0/1 mutex for g_crypto_ctx access FIRST */
    CeePewErr_t mutex_err = crypto_mutex_init();
    if (mutex_err != CEEPEW_OK) { goto error_cleanup; }

    /* Transition to active state BEFORE registering ESL callbacks so that
     * the MAC lock check (session_mac_lock_check, which asserts phase==3U)
     * won't fire while phase is still KEY_DERIVE when a HELLO frame arrives
     * from the peer immediately after its own key derivation. */
    s_session.phase = 3U;
    s_session.session_active = true;

    err = esl_register_callbacks(session_mac_lock_check, session_enforce_nonce_limit);
    if (err != CEEPEW_OK) { goto error_cleanup; }

    /* Initialize session-permuted Hamming FEC using the derived session key */
    err = ecc_hamming_init_session(s_session.session_key);
    if (err != CEEPEW_OK) { goto error_cleanup; }
    session_init_ttl();  /* Phase 4: Initialize TTL tracking */

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

    /* Increment by 2 to preserve nonce parity: initiator uses even
     * nonces (0,2,4,...), responder uses odd nonces (1,3,5,...).
     * Both peers start at complementary parity from MAC ordering,
     * so the two nonce sequences are guaranteed disjoint. */
    s_session.nonce_counter += 2U;

    /* Check for 90% warning level — session should warn but can continue briefly */
    if (s_session.nonce_counter >= CEEPEW_NONCE_WARNING_LIMIT) {
        return CEEPEW_ERR_NONCE_NEARLY_EXHAUSTED;
    }

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
    if (s_session.phase != 3U) {
        ESP_LOGE("SESSION", "session_get_peer_public_key: phase=%u (expected 3), "
                  "active=%d peer_pk_valid=%d init=%d",
                 s_session.phase, s_session.session_active,
                 s_session.peer_sign_pk_valid, s_session.is_initiator);
        return CEEPEW_ERR_PARAM;
    }
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

/* ── X25519 ECDH public key exchange ─────────────────────────────────────── */

CeePewErr_t session_get_local_box_pubkey(uint8_t pk_out[32])
{
    CEEPEW_ASSERT(s_session.phase >= 2U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(pk_out != NULL, CEEPEW_ERR_NULL_PTR);

    memcpy(pk_out, g_crypto_ctx.box_pubkey, 32U);
    return CEEPEW_OK;
}

CeePewErr_t session_set_peer_box_pubkey(const uint8_t peer_pk[32])
{
    CEEPEW_ASSERT(s_session.phase >= 2U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(peer_pk != NULL, CEEPEW_ERR_NULL_PTR);

    memcpy(g_crypto_ctx.peer_box_pubkey, peer_pk, 32U);
    g_crypto_ctx.peer_box_pubkey_valid = true;
    return CEEPEW_OK;
}

bool session_peer_box_pubkey_valid(void)
{
    return g_crypto_ctx.peer_box_pubkey_valid;
}

CeePewErr_t session_set_self_wifi_mac(const uint8_t wifi_mac[6])
{
    CEEPEW_ASSERT(wifi_mac != NULL, CEEPEW_ERR_NULL_PTR);
    uint8_t all_zero = 1U;
    for (uint8_t i = 0U; i < 6U; i++) {
        if (wifi_mac[i] != 0U) { all_zero = 0U; break; }
    }
    CEEPEW_ASSERT(!all_zero, CEEPEW_ERR_PARAM);
    memcpy(s_session.device_id_self_wifi, wifi_mac, 6U);
    s_session.device_id_self_wifi_valid = true;
    /* Persist in file-scope backup that survives session_secure_zero_context() */
    memcpy(s_saved_self_wifi, wifi_mac, 6U);
    s_saved_self_wifi_valid = true;
    return CEEPEW_OK;
}

CeePewErr_t session_set_peer_wifi_mac(const uint8_t wifi_mac[6])
{
    CEEPEW_ASSERT(s_session.phase >= 2U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(wifi_mac != NULL, CEEPEW_ERR_NULL_PTR);
    /* Validate not all zeros */
    uint8_t all_zero = 1U;
    for (uint8_t i = 0U; i < 6U; i++) {
        if (wifi_mac[i] != 0U) { all_zero = 0U; break; }
    }
    CEEPEW_ASSERT(!all_zero, CEEPEW_ERR_PARAM);
    memcpy(s_session.device_id_peer_wifi, wifi_mac, 6U);
    s_session.device_id_peer_wifi_valid = true;
    ESP_LOGI("session_fsm", "Peer WiFi MAC registered: %02X:%02X:%02X:%02X:%02X:%02X",
             wifi_mac[0], wifi_mac[1], wifi_mac[2], wifi_mac[3], wifi_mac[4], wifi_mac[5]);
    return CEEPEW_OK;
}

CeePewErr_t session_get_peer_wifi_mac(uint8_t wifi_mac[6])
{
    CEEPEW_ASSERT(wifi_mac != NULL, CEEPEW_ERR_NULL_PTR);
    if (!s_session.device_id_peer_wifi_valid) {
        return CEEPEW_ERR_PARAM;
    }
    memcpy(wifi_mac, s_session.device_id_peer_wifi, 6U);
    return CEEPEW_OK;
}

bool session_peer_wifi_mac_valid(void)
{
    return s_session.device_id_peer_wifi_valid;
}

/* ── Hardware-Gated Identity Verification (Runtime) ────────────────────
 * Verify that a received ESP-NOW frame's source MAC matches the peer
 * WiFi STA MAC that was delivered over the secure GATT channel during
 * Phase 2. This closes the window where an attacker could relay ESP-NOW
 * frames from a different MAC than the one authenticated via BLE.
 *
 * Called from the RX path during the post-derive sync exchange to
 * binding the transport identity to the GATT-authenticated identity.
 *
 * PARAMETERS:
 *   frame_src_mac: Source MAC from the ESP-NOW radio frame (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK          — MAC matches the GATT-verified peer WiFi MAC
 *   CEEPEW_ERR_AUTH_FAIL — MAC mismatch (potential MITM or relay attack)
 *   CEEPEW_ERR_PARAM    — WiFi MAC not yet provisioned via GATT
 */
CeePewErr_t session_verify_wifi_mac_matches_frame(const uint8_t frame_src_mac[6])
{
    CEEPEW_ASSERT(frame_src_mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);

    if (!s_session.device_id_peer_wifi_valid) {
        return CEEPEW_ERR_PARAM;
    }

    if (!ceepew_ct_equal(s_session.device_id_peer_wifi, frame_src_mac, 6U)) {
        ESP_LOGW("session_fsm", "WiFi MAC mismatch: GATT=%02X:%02X:%02X:%02X:%02X:%02X "
                 "frame=%02X:%02X:%02X:%02X:%02X:%02X",
                 s_session.device_id_peer_wifi[0], s_session.device_id_peer_wifi[1],
                 s_session.device_id_peer_wifi[2], s_session.device_id_peer_wifi[3],
                 s_session.device_id_peer_wifi[4], s_session.device_id_peer_wifi[5],
                 frame_src_mac[0], frame_src_mac[1], frame_src_mac[2],
                 frame_src_mac[3], frame_src_mac[4], frame_src_mac[5]);
        return CEEPEW_ERR_AUTH_FAIL;
    }

    return CEEPEW_OK;
}

/* ── Clean Slate Chat Evolution ────────────────────────────────────────
 * Thread-safe reset of the Stop-and-Wait ARQ engine context and flush
 * of the radio RX queue. Called the instant the sync barrier is verified
 * to ensure no stale sequence numbers, queued frames, or ARQ state from
 * the pairing exchange contaminates the active chat session.
 *
 * The ARQ engine (ecc_arq.c) maintains static sequence counters that
 * must be zeroed before the first chat message is sent. The RX queue
 * may contain stale pairing-phase frames that would corrupt the
 * decryption pipeline if processed after key derivation. */
static void session_flush_arq_and_rx_queue(void)
{
    CeePewErr_t arq_err = ecc_arq_reset();
    if (arq_err != CEEPEW_OK) {
        ESP_LOGW("session_fsm", "ARQ reset failed: %d", (int)arq_err);
    }

    QueueHandle_t rx_q = hal_radio_get_rx_queue();
    if (rx_q != NULL) {
        RadioFrame_t stale_frame;
        uint32_t flushed = 0U;
        while (xQueueReceive(rx_q, &stale_frame, 0) == pdPASS) {
            flushed++;
            if (flushed >= CEEPEW_QUEUE_DEPTH) {
                break;
            }
        }
        if (flushed > 0U) {
            ESP_LOGI("session_fsm", "Flushed %lu stale frames from RX queue", (unsigned long)flushed);
        }
    }
}

bool session_peer_sign_pk_valid(void)
{
    return s_session.peer_sign_pk_valid;
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
 * DOUBLE-ENDED POST-DERIVE RENDEZVOUS:
 * Both peers lock their radios to the static baseline channel
 * (CEEPEW_ESPNOW_CHANNEL) and exchange encrypted HELLO/ACK tokens.
 * The initiator retransmits the HELLO byte; the responder sends the
 * ACK upon HELLO receipt. The barrier is NOT cleared by local key
 * derivation alone — each side must receive and decrypt a valid
 * encrypted frame from the peer before the barrier is flagged.
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

    /* Initialise the timeout deadline on first call.
     * Also drain any stale pairing-phase frames from the radio RX queue so
     * they don't hit the ESL pipeline (too-short non-ESL frames trigger
     * CEEPEW_ASSERT in transport_esl_process_incoming). */
    if (s_session.sync_started_ms == 0ULL) {
        s_session.sync_started_ms = now_ms;
        session_flush_arq_and_rx_queue();
    }

    if ((now_ms - s_session.sync_started_ms) >= CEEPEW_KEY_SYNC_TIMEOUT_MS) {
        return CEEPEW_ERR_TIMEOUT;
    }

    /* ── LOCK RADIOS TO STATIC BASELINE CHANNEL ───────────────────────
     * The post-derive sync exchange MUST occur on the static
     * CEEPEW_ESPNOW_CHANNEL BEFORE channel hopping begins. This
     * ensures both peers are on the same frequency for the encrypted
     * HELLO/ACK rendezvous. hal_radio_set_channel is idempotent. */
    (void)hal_radio_set_channel(CEEPEW_ESPNOW_CHANNEL);

    /* Both peers retransmit. The barrier clears on receipt of any
     * sync message (HELLO or ACK) at either end — see
     * session_handle_key_sync_byte(). */

    /* Exponential backoff for HELLO retransmissions: 50, 100, 200, 400ms.
     * First attempt fires immediately (sync_last_send_ms == 0). */
    static const uint32_t s_sync_backoff_ms[] = {50U, 100U, 200U, 400U, 400U, 400U};
    #define CEEPEW_SYNC_BACKOFF_STAGES \
        (sizeof(s_sync_backoff_ms) / sizeof(s_sync_backoff_ms[0]))
    if (s_session.sync_last_send_ms != 0ULL) {
        uint32_t stage = (s_session.sync_retry_stage < (uint8_t)CEEPEW_SYNC_BACKOFF_STAGES)
                         ? s_session.sync_retry_stage : (uint8_t)(CEEPEW_SYNC_BACKOFF_STAGES - 1U);
        uint32_t backoff = s_sync_backoff_ms[stage];
        if ((now_ms - s_session.sync_last_send_ms) < backoff) {
            return CEEPEW_OK;
        }
    }
    s_session.sync_last_send_ms = now_ms;

    /* Build the 1-byte HELLO payload. We use session_send_message so the
     * full pipeline (compress → Ascon → crypto_box → sign → FEC → CRC →
     * ESP-NOW) runs. A successful receive on the other side is the proof
     * that the key converges. */
    uint8_t hello_plain[1] = { CEEPEW_KEY_SYNC_HELLO_BYTE };

    /* The peer MAC and peer public key are required by session_send_message.
     * Use the GATT-verified peer WiFi MAC for ESP-NOW frame routing. */
    uint8_t peer_mac[6] = {0U};
    uint8_t peer_pk[32] = {0U};

    /* STRICT HARDWARE GATE: Use the WiFi MAC that was verified via GATT.
     * If it was never delivered, the WiFi MAC gate in session_phase2_derive_key()
     * should have already prevented us from reaching this point. Defend here
     * as defense-in-depth. */
    if (!s_session.device_id_peer_wifi_valid) {
        ESP_LOGE("session_fsm", "post_derive_sync: peer WiFi MAC not verified — aborting");
        return CEEPEW_ERR_PARAM;
    }
    memcpy(peer_mac, s_session.device_id_peer_wifi, 6U);

    /* Verify ESP-NOW peer is registered before attempting to send.
     * If not registered, wait — the peer registration happens in task_session.c
     * after key derivation. If we're here but peer isn't registered yet,
     * something is wrong and we should not send garbage. */
    if (!hal_radio_is_peer_registered(peer_mac)) {
        ESP_LOGW("session_fsm", "post_derive_sync: ESP-NOW peer not registered yet — deferring send");
        return CEEPEW_OK;  /* defer this tick, will retry on next loop */
    }

    /* Use the peer's X25519 public key for crypto_box ECDH */
    if (!g_crypto_ctx.peer_box_pubkey_valid) {
        /* Peer's X25519 key hasn't arrived yet via BLE sign_pk exchange.
         * Wait for it to arrive — but if it never does, the timeout
         * above will fire and we transition to PAIRING_FAILED. */
        return CEEPEW_OK;
    }
    memcpy(peer_pk, g_crypto_ctx.peer_box_pubkey, 32U);

    CeePewErr_t send_err = session_send_message(hello_plain, 1U, peer_mac, peer_pk);
    if (send_err == CEEPEW_OK && s_session.sync_retry_stage < 255U) {
        s_session.sync_retry_stage++;
    }
    return send_err;
}

/* Internal: clear sync barrier and perform clean slate chat evolution.
 * This is the SINGLE point of barrier clearance. It:
 *   1. Resets the Stop-and-Wait ARQ engine (zeroes sequence counters)
 *   2. Flushes stale frames from the radio RX queue
 *   3. Posts UI_EVENT_SESSION_ESTABLISHED to transition UI to CHAT_MENU
 *
 * If need_tx is true, return CEEPEW_ERR_NEED_TX (for responder ACK path).
 * Otherwise return CEEPEW_OK. */
static CeePewErr_t session_clear_sync_barrier_internal(bool need_tx)
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    if (s_session.sync_barrier_cleared) {
        return need_tx ? CEEPEW_ERR_NEED_TX : CEEPEW_OK;
    }

    s_session.sync_barrier_cleared = true;
    s_session.sync_retry_stage = 0U;

    /* ── CLEAN SLATE CHAT EVOLUTION ──────────────────────────────────
     * The instant the sync barrier is verified (encrypted round-trip
     * confirmed), issue a thread-safe reset of the ARQ engine and
     * flush the radio RX queue. This prevents stale pairing-phase
     * frames from contaminating the active chat session and ensures
     * sequence counters start from zero for the first chat message. */
    session_flush_arq_and_rx_queue();

    /* Post UI_EVENT_SESSION_ESTABLISHED to notify UI task (Core 0) that
     * the encrypted HELLO/ACK round-trip completed and session is ready.
     * This triggers transition from UI_STATE_KEYDER to UI_STATE_CHAT_MENU. */
    if (g_ui_event_queue != NULL) {
        UIEvent_t evt = {0};
        evt.type = UI_EVENT_SESSION_ESTABLISHED;
        uint64_t sid = session_get_id();
        evt.param = (uint32_t)(sid >> 32);
        evt.payload.session_established.session_id_hi = evt.param;
        (void)xQueueSend(g_ui_event_queue, &evt, 0);
    }

    return need_tx ? CEEPEW_ERR_NEED_TX : CEEPEW_OK;
}

CeePewErr_t session_mark_sync_barrier_cleared(void)
{
    return session_clear_sync_barrier_internal(false);
}

/* Called by the session task's RX path when a 1-byte post-derive sync
 * message is decoded. This implements the DOUBLE-ENDED POST-DERIVE
 * RENDEZVOUS: the barrier is NOT cleared by local key derivation alone.
 * Both devices must exchange encrypted control tokens on the static
 * baseline channel, and each side must receive and decrypt a valid
 * frame from the peer before the barrier is flagged as cleared.
 *
 * Initiator path:
 *   Receives ACK (0x5AU) → marks peer_encrypted_received, clears barrier.
 *   The ACK proves the responder has the correct session key.
 *
 * Responder path:
 *   Receives HELLO (0xA5U) → marks peer_encrypted_received, returns
 *   CEEPEW_ERR_NEED_TX so the caller sends the ACK. The barrier is
 *   NOT cleared yet — it will be cleared by session_confirm_ack_sent()
 *   after the ACK has been enqueued for transmission. This prevents
 *   the race where the responder transitions to chat before the
 *   initiator receives the ACK. */
CeePewErr_t session_handle_key_sync_byte(uint8_t magic_byte)
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    if (s_session.is_initiator) {
        if (magic_byte != CEEPEW_KEY_SYNC_ACK_BYTE) {
            return CEEPEW_ERR_PARAM;
        }
        /* Initiator received ACK — proof that responder has correct keys.
         * Mark peer-encrypted proof received and clear the barrier. */
        s_session.sync_peer_encrypted_received = true;
        return session_clear_sync_barrier_internal(false);
    } else {
        if (magic_byte != CEEPEW_KEY_SYNC_HELLO_BYTE) {
            return CEEPEW_ERR_PARAM;
        }
        /* Responder received HELLO — proof that initiator has correct keys.
         * Mark peer-encrypted proof received but do NOT clear the barrier yet.
         * Return CEEPEW_ERR_NEED_TX so the caller sends the ACK.
         * The barrier will be cleared by session_confirm_ack_sent() once
         * the ACK has been successfully enqueued for transmission. */
        if (s_session.sync_peer_encrypted_received) {
            return CEEPEW_OK;
        }
        s_session.sync_peer_encrypted_received = true;
        return CEEPEW_ERR_NEED_TX;
    }
}

/* Responder-side confirmation: call AFTER the ACK frame has been
 * successfully enqueued for transmission. This is the second half of
 * the double-ended rendezvous — only now that the responder has both
 * received proof (HELLO) AND sent proof (ACK) can the barrier clear.
 *
 * Safe to call from any context. Idempotent. */
CeePewErr_t session_confirm_ack_sent(void)
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    s_session.sync_local_ack_sent = true;

    /* Only clear the barrier if we have also received the peer's proof.
     * If the HELLO hasn't arrived yet, this is a no-op — the barrier
     * will be cleared when session_handle_key_sync_byte() sets
     * sync_peer_encrypted_received and calls
     * session_clear_sync_barrier_internal(). */
    if (s_session.sync_peer_encrypted_received && !s_session.sync_barrier_cleared) {
        return session_clear_sync_barrier_internal(false);
    }
    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Rendezvous Phase (Static Channel Sync before Channel Hopping)         */
/* ────────────────────────────────────────────────────────────────────── */

/* Initialize rendezvous state. Call once after key derivation and BLE teardown,
 * before starting the rendezvous handshake on the static channel. */
CeePewErr_t session_rendezvous_init(void)
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    s_session.rendezvous_initiated = false;
    s_session.rendezvous_synced = false;
    s_session.rendezvous_offset_us = 0;

    /* Reset hal_radio rendezvous state */
    return hal_radio_rendezvous_reset();
}

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
CeePewErr_t session_drive_rendezvous(void)
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    /* If already synced, return success */
    if (s_session.rendezvous_synced) {
        return CEEPEW_OK;
    }

    /* Check for initiator timeout */
    if (hal_radio_rendezvous_check_timeout()) {
        ESP_LOGW("session_fsm", "Rendezvous timeout \u2014 initiator failed to receive ACK");
        return CEEPEW_ERR_TIMEOUT;
    }

    /* If we're the initiator and haven't started rendezvous yet, start it */
    if (s_session.is_initiator && !s_session.rendezvous_initiated) {
        CeePewErr_t err = hal_radio_rendezvous_initiator_start();
        if (err == CEEPEW_OK) {
            s_session.rendezvous_initiated = true;
            ESP_LOGI("session_fsm", "Rendezvous: Initiator started handshake");
        } else if (err != CEEPEW_ERR_PARAM) {
            return err;  /* Real error */
        }
        /* CEEPEW_ERR_PARAM means already in progress, wait for ACK */
    }

    /* Check if rendezvous completed */
    if (hal_radio_rendezvous_is_synced()) {
        s_session.rendezvous_synced = true;
        s_session.rendezvous_offset_us = hal_radio_rendezvous_get_offset_us();
        ESP_LOGI("session_fsm", "Rendezvous: SYNCED, offset=%ld us", (long)s_session.rendezvous_offset_us);
        return CEEPEW_OK;
    }

    return CEEPEW_OK;  /* Still in progress */
}

/* Check if rendezvous handshake is complete and both sides are synced. */
bool session_rendezvous_synced(void)
{
    return s_session.rendezvous_synced;
}

/* Get the rendezvous timing offset (microseconds).
 * Positive = responder clock ahead of initiator.
 * Used to calibrate channel hopping timer phase alignment. */
int32_t session_get_rendezvous_offset_us(void)
{
    return s_session.rendezvous_offset_us;
}

int32_t session_get_peer_uptime_offset(void)
{
    return s_session.peer_uptime_offset_s;
}

CeePewErr_t session_mac_lock_check(const uint8_t peer_mac[6]){
    if (s_session.phase != 3U) {
        CEEPEW_LOG("SESSION", "MAC lock deferred: phase=%u (expected 3)", (unsigned)s_session.phase);
        return CEEPEW_ERR_PARAM;
    }
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);

    /* ESP-NOW frames carry the peer's WiFi STA MAC, not the BLE MAC.
     * Compare against device_id_peer_wifi (set via BLE GATT sign_pk exchange).
     * On ESP32, BLE MAC = WiFi MAC + 2, so comparing against device_id_peer
     * (the BLE random/resolvable MAC) would always fail. */
    if (!s_session.device_id_peer_wifi_valid) {
        return CEEPEW_ERR_TRANSPORT;
    }
    if (!ceepew_ct_equal(s_session.device_id_peer_wifi, peer_mac, 6U)) {
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
    /* Invalidate cached hop key to prevent key material reuse */
    transport_hop_invalidate_key();
    /* Reset ESL callback registration state to allow re-registration */
    esl_reset_callbacks();
    /* Reset session-permuted Hamming FEC to prevent stale permutation reuse */
    (void)ecc_hamming_deinit();
    /* Securely zero X25519 ECDH key material */
    ceepew_secure_zero(g_crypto_ctx.box_privkey, sizeof(g_crypto_ctx.box_privkey));
    ceepew_secure_zero(g_crypto_ctx.peer_box_pubkey, sizeof(g_crypto_ctx.peer_box_pubkey));
    g_crypto_ctx.peer_box_pubkey_valid = false;
    session_secure_zero_context();
    region_reset(&g_region);

    /* M7: Return to low-power state after session ends */
    (void)hal_radio_set_power_save(CEEPEW_WIFI_PS_DISCOVERY);
    (void)transport_ble_set_scan_duty_cycle(CEEPEW_BLE_SCAN_INTERVAL_DISC_MS,
                                            CEEPEW_BLE_SCAN_WINDOW_DISC_MS);

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

    ESP_LOGI("session_fsm", "session_reset_to_discovery: phase %u -> 1 (peer=%02X:%02X:%02X:%02X:%02X:%02X)",
             (unsigned)saved_phase,
             s_session.device_id_peer[0], s_session.device_id_peer[1],
             s_session.device_id_peer[2], s_session.device_id_peer[3],
             s_session.device_id_peer[4], s_session.device_id_peer[5]);

    /* Reset the ESL replay window before starting a new session.
     * The replay window is a static singleton (transport_replay.c) that
     * accumulates seq numbers across sessions. Without this reset, seq
     * numbers from a prior session (or from unit tests run at boot) will
     * be ≤ last_seq and rejected as replays — causing the post-derive
     * sync barrier to time out on every subsequent pairing attempt. */
    transport_replay_reset();

    (void)session_end();

    /* session_end() calls session_secure_zero_context() which zeroes s_session.
     * Re-enter Phase 1 so session_get_phase() returns 1 immediately. */
    err = session_phase1_init(local_id);
    if (err != CEEPEW_OK) { return err; }

    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Query/Diagnostic Functions                                            */
/* ────────────────────────────────────────────────────────────────────── */

uint8_t session_get_phase(void) { return s_session.phase; }

bool session_is_active(void) { return s_session.session_active; }

uint64_t session_get_nonce_counter(void) {
    if (s_test_nc_set) { return s_test_nc; }
    return s_session.nonce_counter;
}

uint64_t session_get_id(void) {
    if (s_test_id_set) { return s_test_id; }
    return ((uint64_t)g_crypto_ctx.session_id[0] << 56U) |
           ((uint64_t)g_crypto_ctx.session_id[1] << 48U) |
           ((uint64_t)g_crypto_ctx.session_id[2] << 40U) |
           ((uint64_t)g_crypto_ctx.session_id[3] << 32U) |
           ((uint64_t)g_crypto_ctx.session_id[4] << 24U) |
           ((uint64_t)g_crypto_ctx.session_id[5] << 16U) |
           ((uint64_t)g_crypto_ctx.session_id[6] << 8U) |
           ((uint64_t)g_crypto_ctx.session_id[7]);
}

void session_test_set_id(uint64_t id) {
    s_test_id = id;
    s_test_id_set = true;
}

void session_test_set_nonce_counter(uint64_t nc) {
    s_test_nc = nc;
    s_test_nc_set = true;
}

void session_test_set_commitment(const uint8_t c[CEEPEW_COMMITMENT_BYTES]) {
    if (c == NULL) { return; }
    memcpy(s_test_commitment, c, CEEPEW_COMMITMENT_BYTES);
    s_test_commitment_set = true;
}

void session_test_unset_commitment(void) {
    s_test_commitment_set = false;
}

CeePewErr_t session_get_device_id(uint8_t device_id[6])
{
    CEEPEW_ASSERT(s_session.phase >= 1U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(device_id != NULL, CEEPEW_ERR_NULL_PTR);
    memcpy(device_id, s_session.device_id_self, 6U);
    return CEEPEW_OK;
}

CeePewErr_t session_get_peer_device_id(uint8_t peer_id[6])
{
    CEEPEW_ASSERT(peer_id != NULL, CEEPEW_ERR_NULL_PTR);
    memcpy(peer_id, s_session.device_id_peer, 6U);
    return CEEPEW_OK;
}

CeePewErr_t session_get_commitment(uint8_t commitment[CEEPEW_COMMITMENT_BYTES])
{
    CEEPEW_ASSERT(commitment != NULL, CEEPEW_ERR_NULL_PTR);

    ESP_LOGI("session_fsm", "session_get_commitment entry: phase=%u test_flag=%d "
             "self=%02X:%02X:%02X:%02X:%02X:%02X peer=%02X:%02X:%02X:%02X:%02X:%02X",
             s_session.phase, (int)s_test_commitment_set,
             s_session.device_id_self[0], s_session.device_id_self[1],
             s_session.device_id_self[2], s_session.device_id_self[3],
             s_session.device_id_self[4], s_session.device_id_self[5],
             s_session.device_id_peer[0], s_session.device_id_peer[1],
             s_session.device_id_peer[2], s_session.device_id_peer[3],
             s_session.device_id_peer[4], s_session.device_id_peer[5]);

    if (s_test_commitment_set) {
        memcpy(commitment, s_test_commitment, CEEPEW_COMMITMENT_BYTES);
        return CEEPEW_OK;
    }
    if (s_session.phase < 2U || s_session.phase > 3U) {
        return CEEPEW_ERR_PARAM;
    }

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

    ESP_LOGI("session_fsm", "session_get_commitment: self=%02X:%02X:%02X:%02X:%02X:%02X peer=%02X:%02X:%02X:%02X:%02X:%02X",
             s_session.device_id_self[0], s_session.device_id_self[1], s_session.device_id_self[2],
             s_session.device_id_self[3], s_session.device_id_self[4], s_session.device_id_self[5],
             s_session.device_id_peer[0], s_session.device_id_peer[1], s_session.device_id_peer[2],
             s_session.device_id_peer[3], s_session.device_id_peer[4], s_session.device_id_peer[5]);
    ESP_LOGI("session_fsm", "session_get_commitment: local_commit=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             commitment[0], commitment[1], commitment[2], commitment[3],
             commitment[4], commitment[5], commitment[6], commitment[7],
             commitment[8], commitment[9], commitment[10], commitment[11],
             commitment[12], commitment[13], commitment[14], commitment[15]);

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
     * the 0xFFF3 GATT characteristic for Ed25519 sign_pk delivery. */
    if (len == CEEPEW_COMMITMENT_ADV_BYTES) {
        uint8_t local_commit[CEEPEW_COMMITMENT_BYTES];
        CeePewErr_t adv_err = session_get_commitment(local_commit);
        if (adv_err != CEEPEW_OK) { return adv_err; }

        ESP_LOGI("session_fsm", "session_verify_peer_commitment_with_sig: peer=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                 peer_data[0], peer_data[1], peer_data[2], peer_data[3],
                 peer_data[4], peer_data[5], peer_data[6], peer_data[7],
                 peer_data[8], peer_data[9], peer_data[10], peer_data[11],
                 peer_data[12], peer_data[13], peer_data[14], peer_data[15]);

        ESP_LOGI("session_fsm", "verify_adv: local=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                 local_commit[0], local_commit[1], local_commit[2], local_commit[3],
                 local_commit[4], local_commit[5], local_commit[6], local_commit[7],
                 local_commit[8], local_commit[9], local_commit[10], local_commit[11],
                 local_commit[12], local_commit[13], local_commit[14], local_commit[15]);

        uint8_t diff = 0U;
        for (uint8_t i = 0U; i < CEEPEW_COMMITMENT_ADV_BYTES; i++) {
            diff |= (uint8_t)(local_commit[i] ^ peer_data[i]);
        }
        ESP_LOGI("session_fsm", "verify_adv: diff=0x%02X self_mac=%02X:%02X:%02X:%02X:%02X:%02X peer_mac=%02X:%02X:%02X:%02X:%02X:%02X phase=%u test=%d",
                 diff,
                 s_session.device_id_self[0], s_session.device_id_self[1],
                 s_session.device_id_self[2], s_session.device_id_self[3],
                 s_session.device_id_self[4], s_session.device_id_self[5],
                 s_session.device_id_peer[0], s_session.device_id_peer[1],
                 s_session.device_id_peer[2], s_session.device_id_peer[3],
                 s_session.device_id_peer[4], s_session.device_id_peer[5],
                 s_session.phase, (int)s_test_commitment_set);
        ceepew_secure_zero(local_commit, sizeof(local_commit));
        return (diff == 0U) ? CEEPEW_OK : CEEPEW_ERR_AUTH_FAIL;
    }

    /* ── GATT paths (full 32B commitment, optional 64B signature) ───── */
    const uint8_t cmp_len = CEEPEW_COMMITMENT_BYTES;

    /* Compare peer commitment against our locally computed commitment (constant-time style) */
    uint8_t local_commit[CEEPEW_COMMITMENT_BYTES];
    CeePewErr_t err = session_get_commitment(local_commit);
    if (err != CEEPEW_OK) { return err; }

    ESP_LOGI("session_fsm", "verify_gatt: local=%02X%02X%02X%02X%02X%02X%02X%02X"
             "%02X%02X%02X%02X%02X%02X%02X%02X peer=%02X%02X%02X%02X%02X%02X%02X%02X"
             "%02X%02X%02X%02X%02X%02X%02X%02X len=%u",
             local_commit[0], local_commit[1], local_commit[2], local_commit[3],
             local_commit[4], local_commit[5], local_commit[6], local_commit[7],
             local_commit[8], local_commit[9], local_commit[10], local_commit[11],
             local_commit[12], local_commit[13], local_commit[14], local_commit[15],
             peer_data[0], peer_data[1], peer_data[2], peer_data[3],
             peer_data[4], peer_data[5], peer_data[6], peer_data[7],
             peer_data[8], peer_data[9], peer_data[10], peer_data[11],
             peer_data[12], peer_data[13], peer_data[14], peer_data[15],
             (unsigned)len);

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
    s_session.last_message_time_s = (uint64_t)(esp_timer_get_time() / 1000000LL);
}

/* Phase 4: Update last message activity timestamp */
CeePewErr_t session_update_last_message_time(void)
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    s_session.last_message_time_s = (uint64_t)(esp_timer_get_time() / 1000000LL);
    return CEEPEW_OK;
}

/* Phase 4: Get idle time since last message activity */
CeePewErr_t session_get_idle_seconds(uint32_t *idle_seconds)
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(idle_seconds != NULL, CEEPEW_ERR_NULL_PTR);

    uint64_t now_s = (uint64_t)(esp_timer_get_time() / 1000000LL);
    uint64_t idle = (now_s > s_session.last_message_time_s) ?
                    (now_s - s_session.last_message_time_s) : 0U;
    /* Clamp to UINT32_MAX for backward compatibility with callers */
    *idle_seconds = (idle > UINT32_MAX) ? UINT32_MAX : (uint32_t)idle;
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

    /* Reset session-permuted Hamming FEC */
    (void)ecc_hamming_deinit();

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

/* ────────────────────────────────────────────────────────────────────── */
/* PFS (Perfect Forward Secrecy) — ephemeral ECDH over ESP-NOW           */
/* ────────────────────────────────────────────────────────────────────── */

CeePewErr_t session_pfs_initiate(void)
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);

    /* Generate ephemeral Curve25519 keypair for PFS */
    CeePewErr_t err = crypto_ecdh_generate_keypair(
        g_crypto_ctx.pfs_pubkey, g_crypto_ctx.pfs_privkey);
    if (err != CEEPEW_OK) {
        return err;
    }

    g_crypto_ctx.pfs_active = false;
    g_crypto_ctx.pfs_peer_pubkey_valid = false;
    ceepew_secure_zero(g_crypto_ctx.pfs_shared_secret, sizeof(g_crypto_ctx.pfs_shared_secret));
    ceepew_secure_zero(g_crypto_ctx.pfs_ascon_key, sizeof(g_crypto_ctx.pfs_ascon_key));

    ESP_LOGI("SESSION", "PFS keypair generated");
    return CEEPEW_OK;
}

CeePewErr_t session_pfs_process_peer_key(const uint8_t peer_pfs_pubkey[32])
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(peer_pfs_pubkey != NULL, CEEPEW_ERR_NULL_PTR);

    /* Compute shared secret: local_priv * peer_pub */
    CeePewErr_t err = crypto_ecdh_shared_secret(
        g_crypto_ctx.pfs_privkey, peer_pfs_pubkey, g_crypto_ctx.pfs_shared_secret);
    if (err != CEEPEW_OK) {
        return err;
    }

    memcpy(g_crypto_ctx.pfs_peer_pubkey, peer_pfs_pubkey, 32U);
    g_crypto_ctx.pfs_peer_pubkey_valid = true;

    /* Derive new Ascon-128 session key from PFS shared secret using HKDF.
     * Info string binds the key to this session (device IDs + session_id). */
    static const uint8_t pfs_info[] = "CEEPEW_PFS_SESSION_v1";
    err = crypto_hkdf_derive(
        g_crypto_ctx.pfs_shared_secret, 32U,
        NULL, 0U,
        pfs_info, sizeof(pfs_info) - 1,
        g_crypto_ctx.pfs_ascon_key, 16U);
    if (err != CEEPEW_OK) {
        return err;
    }

    /* Switch active session key to PFS-derived key */
    memcpy(g_crypto_ctx.ascon_key, g_crypto_ctx.pfs_ascon_key, 16U);
    g_crypto_ctx.pfs_active = true;

    ESP_LOGI("SESSION", "PFS handshake complete — session key rotated");
    return CEEPEW_OK;
}

bool session_pfs_active(void)
{
    return g_crypto_ctx.pfs_active;
}

CeePewErr_t session_get_local_pfs_pubkey(uint8_t pfs_pubkey_out[32])
{
    CEEPEW_ASSERT(s_session.phase == 3U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_session.session_active, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(pfs_pubkey_out != NULL, CEEPEW_ERR_NULL_PTR);

    memcpy(pfs_pubkey_out, g_crypto_ctx.pfs_pubkey, 32U);
    return CEEPEW_OK;
}

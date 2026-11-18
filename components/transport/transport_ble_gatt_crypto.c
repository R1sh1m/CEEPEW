/* components/transport/transport_ble_gatt_crypto.c
 *
 * GATT Commitment Exchange Crypto Wrapper (Phase 2 Pairing).
 *
 * This is a CRYPTO HELPER for transport_ble.c — NOT a standalone transport.
 * It provides Ascon-128 AEAD encryption/decryption of the sign_pk payload
 * exchanged over the brief GATT connection during pairing.
 *
 * transport_ble.c: Main BLE transport (advertising, scanning, GATT connection).
 *   Calls gatt_crypto_encrypt_with_ids() when writing sign_pk as GATT client.
 *   Calls gatt_crypto_decrypt_with_ids() when reading sign_pk as GATT server.
 *
 * After Phase 3 handoff, this module is no longer used.
 * See transport_espnow.c for the active session transport.
 */

#include "transport_ble_gatt_crypto.h"
#include "ceepew_assert.h"
#include "crypto_ascon.h"
#include "crypto_hkdf.h"
#include "crypto_rng.h"
#include "crypto_sha256.h"
#include "ceepew_security_utils.h"
#include "ceepew_config.h"
#include <string.h>

/* Domain-separation strings. NEVER reuse a value across versions without
 * bumping the suffix — otherwise cross-version key reuse becomes possible. */
static const uint8_t GATT_SALT_LABEL[]    = "CEEPEW_GATT_SALT_v2";
static const uint8_t GATT_INFO_LABEL[]    = "CEEPEW_GATT_SIGN_PK_v2";
static const uint8_t GATT_NONCE_LABEL[]   = "CEEPEW_GATT_NONCE_v2";

#define GATT_SALT_LABEL_LEN  (sizeof(GATT_SALT_LABEL) - 1U)
#define GATT_INFO_LABEL_LEN  (sizeof(GATT_INFO_LABEL) - 1U)
#define GATT_NONCE_LABEL_LEN (sizeof(GATT_NONCE_LABEL) - 1U)

/* Volatile monotonic counter for nonce diversification — increments on
 * every GATT encryption during the current boot session. Conforms to the
 * zero-NVS key policy (restarts at 0 on power-on and is never written to flash).
 * Enforces a fail-closed exhaustion policy at ctr == 255 to strictly prevent
 * integer wraparound under the same (Kboot, boot_salt) pair. */
static uint8_t s_gatt_nonce_ctr = 0U;

/* Per-boot TRNG salt for GATT nonce diversification. Generated once per
 * device boot via crypto_rng_fill() and held in volatile static memory.
 * Transmitted in-band with every GATT ciphertext so the decryptor derives the
 * identical nonce. By the birthday bound, provides a 2^-33 collision bound
 * across 2^16 reboots under identical pairing code. */
static uint8_t s_gatt_boot_salt[GATT_BOOT_SALT_BYTES];
static bool    s_gatt_boot_salt_ready = false;

#ifdef CONFIG_CEEPEW_DEVELOPMENT_MODE
static bool    s_gatt_boot_salt_force_fail = false;

void gatt_crypto_test_set_trng_fail(bool fail)
{
    s_gatt_boot_salt_force_fail = fail;
    if (fail) {
        s_gatt_boot_salt_ready = false;
        ceepew_secure_zero(s_gatt_boot_salt, sizeof(s_gatt_boot_salt));
    }
}
#endif

/* Lazily initialise the per-boot salt from TRNG on first use. Safe to call
 * from any context; subsequent calls are no-ops. */
static CeePewErr_t gatt_crypto_ensure_boot_salt(void)
{
    if (s_gatt_boot_salt_ready) {
        return CEEPEW_OK;
    }
#ifdef CONFIG_CEEPEW_DEVELOPMENT_MODE
    if (s_gatt_boot_salt_force_fail) {
        ceepew_secure_zero(s_gatt_boot_salt, sizeof(s_gatt_boot_salt));
        return CEEPEW_ERR_HW;
    }
#endif
    CeePewErr_t err = crypto_rng_fill(s_gatt_boot_salt, GATT_BOOT_SALT_BYTES);
    if (err != CEEPEW_OK) {
        /* Fail closed: TRNG failure must never fall back to predictable constants,
         * timestamps, or MAC addresses. Zero state and abort pairing immediately. */
        ceepew_secure_zero(s_gatt_boot_salt, sizeof(s_gatt_boot_salt));
        s_gatt_boot_salt_ready = false;
        return err;
    }
    s_gatt_boot_salt_ready = true;
    return CEEPEW_OK;
}

/* Derive the Ascon-128 key and nonce used for the GATT sign_pk write.
 * Both peers run this with the same session_code + sorted device IDs
 * and the same wire_ctr + boot_salt values, producing identical key + nonce.
 *
 * wire_ctr:      Transmitted counter byte (read by decrypt from in[0],
 *                read by encrypt from the local s_gatt_nonce_ctr before
 *                incrementing). Both peers use the same value so the
 *                nonce matches.
 * boot_salt:     8-byte per-boot TRNG salt from the encrypting device,
 *                transmitted in-band (read by decrypt from in[1..8]).
 *                Changes on every device reboot, preventing cross-reboot
 *                nonce reuse when ctr restarts at 0.
 * id_a, id_b:    6-byte device IDs (sorted lexicographically by the caller)
 * key_out:       16-byte Ascon key
 * nonce_out:     16-byte Ascon nonce
 */
static CeePewErr_t gatt_crypto_derive_key_and_nonce(
    const uint8_t session_code[32],
    const uint8_t id_a[6],
    const uint8_t id_b[6],
    uint8_t wire_ctr,
    const uint8_t boot_salt[GATT_BOOT_SALT_BYTES],
    uint8_t key_out[GATT_KEY_BYTES],
    uint8_t nonce_out[GATT_NONCE_BYTES])
{
    CEEPEW_ASSERT(session_code != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(id_a != NULL && id_b != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(boot_salt != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(key_out != NULL && nonce_out != NULL, CEEPEW_ERR_NULL_PTR);

    /* ── salt = SHA256("CEEPEW_GATT_SALT_v2") ── */
    uint8_t salt[CEEPEW_SHA256_BYTES];
    CeePewErr_t err = crypto_sha256_compute(GATT_SALT_LABEL,
                                            GATT_SALT_LABEL_LEN,
                                            salt);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(salt, sizeof(salt));
        return err;
    }

    /* ── info = "CEEPEW_GATT_SIGN_PK_v2" || idA || idB (already sorted by caller) ── */
    uint8_t info[GATT_INFO_LABEL_LEN + 6U + 6U];
    memcpy(&info[0], GATT_INFO_LABEL, GATT_INFO_LABEL_LEN);
    memcpy(&info[GATT_INFO_LABEL_LEN], id_a, 6U);
    memcpy(&info[GATT_INFO_LABEL_LEN + 6U], id_b, 6U);

    /* ── key = HKDF-SHA256(session_code, salt, info)[:16] ── */
    err = crypto_hkdf_derive(session_code, 32U,
                             salt, (uint8_t)sizeof(salt),
                             info, (uint8_t)sizeof(info),
                             key_out, GATT_KEY_BYTES);
    ceepew_secure_zero(salt, sizeof(salt));
    ceepew_secure_zero(info, sizeof(info));
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(key_out, GATT_KEY_BYTES);
        return err;
    }

    /* ── nonce = SHA256("CEEPEW_GATT_NONCE_v2" || session_code || boot_salt || wire_ctr)[:16] ──
     *
     * boot_salt changes on every device reboot, so even if wire_ctr
     * restarts at 0 after a reboot and the session code is unchanged,
     * the (key, nonce) pair differs from any previous boot. Within a
     * single boot, wire_ctr only increases, so consecutive pairings
     * between the same two devices with the same code also produce
     * distinct nonces. */
    uint8_t nonce_input[GATT_NONCE_LABEL_LEN + 32U + GATT_BOOT_SALT_BYTES + 1U];
    uint8_t *cursor = nonce_input;
    memcpy(cursor, GATT_NONCE_LABEL, GATT_NONCE_LABEL_LEN);
    cursor += GATT_NONCE_LABEL_LEN;
    memcpy(cursor, session_code, 32U);
    cursor += 32U;
    memcpy(cursor, boot_salt, GATT_BOOT_SALT_BYTES);
    cursor += GATT_BOOT_SALT_BYTES;
    *cursor = wire_ctr;

    uint8_t nonce_full[CEEPEW_SHA256_BYTES];
    err = crypto_sha256_compute(nonce_input, (uint32_t)sizeof(nonce_input),
                                nonce_full);
    ceepew_secure_zero(nonce_input, sizeof(nonce_input));
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(nonce_full, sizeof(nonce_full));
        ceepew_secure_zero(key_out, GATT_KEY_BYTES);
        return err;
    }
    memcpy(nonce_out, nonce_full, GATT_NONCE_BYTES);
    ceepew_secure_zero(nonce_full, sizeof(nonce_full));

    return CEEPEW_OK;
}

/* Sort two 6-byte device IDs lexicographically into (lo, hi). Identical
 * IDs are tolerated — both outputs will be the same value, which still
 * produces a well-defined HKDF info string (the key will be derived
 * consistently on both sides). */
static void gatt_crypto_sort_ids(const uint8_t id_self[6],
                                 const uint8_t id_peer[6],
                                 uint8_t lo_out[6],
                                 uint8_t hi_out[6])
{
    CEEPEW_ASSERT_VOID(id_self != NULL && id_peer != NULL);
    CEEPEW_ASSERT_VOID(lo_out != NULL && hi_out != NULL);

    if (!ceepew_ct_less(id_peer, id_self, 6U)) {
        memcpy(lo_out, id_self, 6U);
        memcpy(hi_out, id_peer, 6U);
    } else {
        memcpy(lo_out, id_peer, 6U);
        memcpy(hi_out, id_self, 6U);
    }
}

CeePewErr_t gatt_crypto_encrypt_with_ids(const uint8_t session_code[32],
                                          const uint8_t id_self[6],
                                          const uint8_t id_peer[6],
                                          const uint8_t plaintext[GATT_PLAINTEXT_BYTES],
                                          uint8_t out[GATT_CRYPTO_TOTAL_BYTES])
{
    CEEPEW_ASSERT(session_code != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(id_self != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(id_peer != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(plaintext != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t key[GATT_KEY_BYTES];
    uint8_t nonce[GATT_NONCE_BYTES];
    uint8_t lo[6];
    uint8_t hi[6];
    uint8_t aad[GATT_COUNTER_BYTES];   /* [ctr][boot_salt] = 9 bytes */
    gatt_crypto_sort_ids(id_self, id_peer, lo, hi);

    /* Ensure per-boot salt is initialised before any encrypt call. */
    CeePewErr_t err = gatt_crypto_ensure_boot_salt();
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(out, GATT_CRYPTO_TOTAL_BYTES);
        return err;
    }

    /* Enforce fail-closed exhaustion policy: the 1-byte volatile counter
     * permits 255 encryptions per boot. At ctr == 255, refuse further
     * encryption to prevent counter wraparound and duplicate nonce derivation
     * under the same (Kboot, boot_salt). A physical reboot is required to
     * sample a fresh TRNG boot_salt and reset the counter. */
    if (s_gatt_nonce_ctr >= 255U) {
        return CEEPEW_ERR_NONCE_EXHAUSTED;
    }

    /* Read the monotonic counter before incrementing. */
    uint8_t wire_ctr = s_gatt_nonce_ctr;
    s_gatt_nonce_ctr++;

    /* Write [ctr (1B) || boot_salt (8B)] prefix directly into wire format at out[0..8] */
    out[0] = wire_ctr;
    memcpy(&out[1], s_gatt_boot_salt, GATT_BOOT_SALT_BYTES);

    /* Build combined AAD: [ctr (1B) || boot_salt (8B)] = 9 bytes. */
    aad[0] = wire_ctr;
    memcpy(&aad[1], s_gatt_boot_salt, GATT_BOOT_SALT_BYTES);

    err = gatt_crypto_derive_key_and_nonce(session_code,
                                           lo, hi,
                                           wire_ctr,
                                           s_gatt_boot_salt,
                                           key, nonce);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(key, sizeof(key));
        ceepew_secure_zero(nonce, sizeof(nonce));
        ceepew_secure_zero(aad, sizeof(aad));
        ceepew_secure_zero(out, GATT_CRYPTO_TOTAL_BYTES);
        return err;
    }

    /* Ascon-128 AEAD with [ctr || boot_salt] as AAD — the plaintext is
     * bound to the key (derived from session_code) and the counter+boot
     * salt are authenticated by the AEAD tag. Wire format:
     * out[0]     = counter, out[1..8] = boot salt,
     * out[9..114] = ciphertext || tag. */
    uint16_t ct_len = GATT_CRYPTO_TOTAL_BYTES - GATT_COUNTER_BYTES;
    err = crypto_ascon_aead_encrypt(key, nonce,
                                    aad, GATT_COUNTER_BYTES,
                                    plaintext, GATT_PLAINTEXT_BYTES,
                                    out + GATT_COUNTER_BYTES, &ct_len);
    ceepew_secure_zero(key, sizeof(key));
    ceepew_secure_zero(nonce, sizeof(nonce));
    ceepew_secure_zero(aad, sizeof(aad));
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(out, GATT_CRYPTO_TOTAL_BYTES);
        return err;
    }
    CEEPEW_ASSERT(ct_len == GATT_CRYPTO_TOTAL_BYTES - GATT_COUNTER_BYTES, CEEPEW_ERR_CRYPTO);
    return CEEPEW_OK;
}

CeePewErr_t gatt_crypto_decrypt_with_ids(const uint8_t session_code[32],
                                          const uint8_t id_self[6],
                                          const uint8_t id_peer[6],
                                          const uint8_t in[GATT_CRYPTO_TOTAL_BYTES],
                                          uint8_t plaintext_out[GATT_PLAINTEXT_BYTES])
{
    CEEPEW_ASSERT(session_code != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(id_self != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(id_peer != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(plaintext_out != NULL, CEEPEW_ERR_NULL_PTR);

    /* The first byte of the wire payload is the counter; bytes 1..8 are the
     * encrypting device's per-boot salt. Both are needed for nonce derivation
     * and are bound as AAD. Extract them before decrypting. */
    uint8_t wire_ctr = in[0];
    uint8_t boot_salt[GATT_BOOT_SALT_BYTES];
    memcpy(boot_salt, &in[1], GATT_BOOT_SALT_BYTES);
    uint8_t aad[GATT_COUNTER_BYTES];
    aad[0] = wire_ctr;
    memcpy(&aad[1], boot_salt, GATT_BOOT_SALT_BYTES);
    const uint8_t *ct_tag = in + GATT_COUNTER_BYTES;

    uint8_t key[GATT_KEY_BYTES];
    uint8_t nonce[GATT_NONCE_BYTES];
    uint8_t lo[6];
    uint8_t hi[6];
    gatt_crypto_sort_ids(id_self, id_peer, lo, hi);

    CeePewErr_t err = gatt_crypto_derive_key_and_nonce(session_code,
                                                       lo, hi,
                                                       wire_ctr,
                                                       boot_salt,
                                                       key, nonce);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(key, sizeof(key));
        ceepew_secure_zero(nonce, sizeof(nonce));
        ceepew_secure_zero(boot_salt, sizeof(boot_salt));
        ceepew_secure_zero(aad, sizeof(aad));
        return err;
    }

    uint16_t pt_len = GATT_PLAINTEXT_BYTES;
    err = crypto_ascon_aead_decrypt(key, nonce,
                                    aad, GATT_COUNTER_BYTES,
                                    ct_tag, GATT_CRYPTO_TOTAL_BYTES - GATT_COUNTER_BYTES,
                                    plaintext_out, &pt_len);
    ceepew_secure_zero(key, sizeof(key));
    ceepew_secure_zero(nonce, sizeof(nonce));
    ceepew_secure_zero(boot_salt, sizeof(boot_salt));
    ceepew_secure_zero(aad, sizeof(aad));
    if (err == CEEPEW_ERR_CRYPTO) {
        /* Ascon returns CRYPTO on tag mismatch — re-map to AUTH_FAIL so
         * the caller (GATTS write handler) can react specifically
         * (force disconnect, bump reconnect_attempts). */
        ceepew_secure_zero(plaintext_out, GATT_PLAINTEXT_BYTES);
        return CEEPEW_ERR_AUTH_FAIL;
    }
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(plaintext_out, GATT_PLAINTEXT_BYTES);
        return err;
    }
    CEEPEW_ASSERT(pt_len == GATT_PLAINTEXT_BYTES, CEEPEW_ERR_CRYPTO);
    return CEEPEW_OK;
}

#ifdef CONFIG_CEEPEW_DEVELOPMENT_MODE
void gatt_crypto_test_set_state(uint8_t ctr, const uint8_t boot_salt[GATT_BOOT_SALT_BYTES])
{
    s_gatt_nonce_ctr = ctr;
    if (boot_salt != NULL) {
        memcpy(s_gatt_boot_salt, boot_salt, GATT_BOOT_SALT_BYTES);
        s_gatt_boot_salt_ready = true;
    } else {
        s_gatt_boot_salt_ready = false;
    }
}
#endif

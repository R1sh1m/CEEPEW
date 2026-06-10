/* components/transport/transport_ble_gatt_crypto.c
 *
 * Implementation of the Hybrid-GATT sign_pk encryption wrapper.
 * See transport_ble_gatt_crypto.h for the threat model and derivation.
 */

#include "transport_ble_gatt_crypto.h"
#include "crypto_ascon.h"
#include "crypto_hkdf.h"
#include "crypto_sha256.h"
#include "ceepew_security_utils.h"
#include "ceepew_config.h"
#include <string.h>

/* Domain-separation strings. NEVER reuse a value across versions without
 * bumping the suffix — otherwise cross-version key reuse becomes possible. */
static const uint8_t GATT_SALT_LABEL[]    = "CEEPEW_GATT_SALT_v1";
static const uint8_t GATT_INFO_LABEL[]    = "CEEPEW_GATT_SIGN_PK_v1";
static const uint8_t GATT_NONCE_LABEL[]   = "CEEPEW_GATT_NONCE_v1";

#define GATT_SALT_LABEL_LEN  (sizeof(GATT_SALT_LABEL) - 1U)
#define GATT_INFO_LABEL_LEN  (sizeof(GATT_INFO_LABEL) - 1U)
#define GATT_NONCE_LABEL_LEN (sizeof(GATT_NONCE_LABEL) - 1U)

/* Derive the Ascon-128 key and nonce used for the GATT sign_pk write.
 * Both peers run this with the same session_code + sorted device IDs
 * and get the same key + nonce.
 *
 * id_a, id_b:    6-byte device IDs (sorted lexicographically by the caller)
 * key_out:       16-byte Ascon key
 * nonce_out:     16-byte Ascon nonce
 */
static CeePewErr_t gatt_crypto_derive_key_and_nonce(
    const uint8_t session_code[32],
    const uint8_t id_a[6],
    const uint8_t id_b[6],
    uint8_t key_out[GATT_KEY_BYTES],
    uint8_t nonce_out[GATT_NONCE_BYTES])
{
    CEEPEW_ASSERT(session_code != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(id_a != NULL && id_b != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(key_out != NULL && nonce_out != NULL, CEEPEW_ERR_NULL_PTR);

    /* ── salt = SHA256("CEEPEW_GATT_SALT_v1") ── */
    uint8_t salt[CEEPEW_SHA256_BYTES];
    CeePewErr_t err = crypto_sha256_compute(GATT_SALT_LABEL,
                                            GATT_SALT_LABEL_LEN,
                                            salt);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(salt, sizeof(salt));
        return err;
    }

    /* ── info = "CEEPEW_GATT_SIGN_PK_v1" || idA || idB (already sorted by caller) ── */
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

    /* ── nonce = SHA256("CEEPEW_GATT_NONCE_v1" || session_code)[:16] ── */
    uint8_t nonce_input[GATT_NONCE_LABEL_LEN + 32U];
    memcpy(&nonce_input[0], GATT_NONCE_LABEL, GATT_NONCE_LABEL_LEN);
    memcpy(&nonce_input[GATT_NONCE_LABEL_LEN], session_code, 32U);

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

    if (memcmp(id_self, id_peer, 6U) <= 0) {
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
                                          const uint8_t plaintext[32],
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
    gatt_crypto_sort_ids(id_self, id_peer, lo, hi);

    CeePewErr_t err = gatt_crypto_derive_key_and_nonce(session_code,
                                                       lo, hi,
                                                       key, nonce);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(key, sizeof(key));
        ceepew_secure_zero(nonce, sizeof(nonce));
        return err;
    }

    /* Ascon-128 AEAD with no associated data — the plaintext is bound
     * to the key (derived from session_code) so a hostile device cannot
     * inject a chosen sign_pk. The wire carries 32B ct + 16B tag = 48B. */
    uint16_t ct_len = 0U;
    err = crypto_ascon_aead_encrypt(key, nonce,
                                    NULL, 0U,
                                    plaintext, 32U,
                                    out, &ct_len);
    ceepew_secure_zero(key, sizeof(key));
    ceepew_secure_zero(nonce, sizeof(nonce));
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(out, GATT_CRYPTO_TOTAL_BYTES);
        return err;
    }
    CEEPEW_ASSERT(ct_len == GATT_CRYPTO_TOTAL_BYTES, CEEPEW_ERR_CRYPTO);
    return CEEPEW_OK;
}

CeePewErr_t gatt_crypto_decrypt_with_ids(const uint8_t session_code[32],
                                          const uint8_t id_self[6],
                                          const uint8_t id_peer[6],
                                          const uint8_t in[GATT_CRYPTO_TOTAL_BYTES],
                                          uint8_t plaintext_out[32])
{
    CEEPEW_ASSERT(session_code != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(id_self != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(id_peer != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(plaintext_out != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t key[GATT_KEY_BYTES];
    uint8_t nonce[GATT_NONCE_BYTES];
    uint8_t lo[6];
    uint8_t hi[6];
    gatt_crypto_sort_ids(id_self, id_peer, lo, hi);

    CeePewErr_t err = gatt_crypto_derive_key_and_nonce(session_code,
                                                       lo, hi,
                                                       key, nonce);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(key, sizeof(key));
        ceepew_secure_zero(nonce, sizeof(nonce));
        return err;
    }

    uint16_t pt_len = 0U;
    err = crypto_ascon_aead_decrypt(key, nonce,
                                    NULL, 0U,
                                    in, GATT_CRYPTO_TOTAL_BYTES,
                                    plaintext_out, &pt_len);
    ceepew_secure_zero(key, sizeof(key));
    ceepew_secure_zero(nonce, sizeof(nonce));
    if (err == CEEPEW_ERR_CRYPTO) {
        /* Ascon returns CRYPTO on tag mismatch — re-map to AUTH_FAIL so
         * the caller (GATTS write handler) can react specifically
         * (force disconnect, bump reconnect_attempts). */
        ceepew_secure_zero(plaintext_out, 32U);
        return CEEPEW_ERR_AUTH_FAIL;
    }
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(plaintext_out, 32U);
        return err;
    }
    CEEPEW_ASSERT(pt_len == 32U, CEEPEW_ERR_CRYPTO);
    return CEEPEW_OK;
}

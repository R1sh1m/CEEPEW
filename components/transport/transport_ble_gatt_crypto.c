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
#include "crypto_ascon.h"
#include "crypto_hkdf.h"
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

/* Monotonic counter for nonce diversification — prevents nonce reuse across
 * GATT writes within the same pairing session. Incremented on every encrypt
 * call. The current value is prepended to the GATT payload and transmitted
 * in-band so the decryptor derives the identical nonce without needing a
 * synchronised local counter. A single byte is sufficient; the counter
 * will never wrap (max 2 retries per pairing). */
static uint8_t s_gatt_nonce_ctr = 0U;

/* Derive the Ascon-128 key and nonce used for the GATT sign_pk write.
 * Both peers run this with the same session_code + sorted device IDs
 * and the same wire_ctr value, producing identical key + nonce.
 *
 * wire_ctr:      Transmitted counter byte (read by decrypt from in[0],
 *                read by encrypt from the local s_gatt_nonce_ctr before
 *                incrementing). Both peers use the same value so the
 *                nonce matches.
 * id_a, id_b:    6-byte device IDs (sorted lexicographically by the caller)
 * key_out:       16-byte Ascon key
 * nonce_out:     16-byte Ascon nonce
 */
static CeePewErr_t gatt_crypto_derive_key_and_nonce(
    const uint8_t session_code[32],
    const uint8_t id_a[6],
    const uint8_t id_b[6],
    uint8_t wire_ctr,
    uint8_t key_out[GATT_KEY_BYTES],
    uint8_t nonce_out[GATT_NONCE_BYTES])
{
    CEEPEW_ASSERT(session_code != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(id_a != NULL && id_b != NULL, CEEPEW_ERR_NULL_PTR);
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

    /* ── nonce = SHA256("CEEPEW_GATT_NONCE_v2" || session_code || wire_ctr)[:16] ── */
    uint8_t nonce_input[GATT_NONCE_LABEL_LEN + 32U + 1U];
    memcpy(&nonce_input[0], GATT_NONCE_LABEL, GATT_NONCE_LABEL_LEN);
    memcpy(&nonce_input[GATT_NONCE_LABEL_LEN], session_code, 32U);
    nonce_input[GATT_NONCE_LABEL_LEN + 32U] = wire_ctr;

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
    gatt_crypto_sort_ids(id_self, id_peer, lo, hi);

    /* Read the monotonic counter before incrementing — this value is
     * prepended to the wire format and used for nonce derivation on
     * both sides. */
    uint8_t wire_ctr = s_gatt_nonce_ctr;
    s_gatt_nonce_ctr++;

    CeePewErr_t err = gatt_crypto_derive_key_and_nonce(session_code,
                                                       lo, hi,
                                                       wire_ctr,
                                                       key, nonce);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(key, sizeof(key));
        ceepew_secure_zero(nonce, sizeof(nonce));
        return err;
    }

    /* Ascon-128 AEAD with the counter byte as AAD — the plaintext (sign_pk ||
     * box_pubkey || wifi_mac) is bound to the key (derived from session_code)
     * and the counter is authenticated by the AEAD tag. Wire format:
     * out[0] = counter, out[1..86] = ciphertext || tag. */
    uint16_t ct_len = GATT_CRYPTO_TOTAL_BYTES - GATT_COUNTER_BYTES;
    err = crypto_ascon_aead_encrypt(key, nonce,
                                    &wire_ctr, GATT_COUNTER_BYTES,
                                    plaintext, GATT_PLAINTEXT_BYTES,
                                    out + GATT_COUNTER_BYTES, &ct_len);
    ceepew_secure_zero(key, sizeof(key));
    ceepew_secure_zero(nonce, sizeof(nonce));
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(out, GATT_CRYPTO_TOTAL_BYTES);
        return err;
    }
    CEEPEW_ASSERT(ct_len == GATT_CRYPTO_TOTAL_BYTES - GATT_COUNTER_BYTES, CEEPEW_ERR_CRYPTO);
    out[0] = wire_ctr;
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

    /* The first byte of the wire payload is the counter used for
     * nonce derivation — extract it before decrypting. */
    uint8_t wire_ctr = in[0];
    const uint8_t *ct_tag = in + GATT_COUNTER_BYTES;

    uint8_t key[GATT_KEY_BYTES];
    uint8_t nonce[GATT_NONCE_BYTES];
    uint8_t lo[6];
    uint8_t hi[6];
    gatt_crypto_sort_ids(id_self, id_peer, lo, hi);

    CeePewErr_t err = gatt_crypto_derive_key_and_nonce(session_code,
                                                       lo, hi,
                                                       wire_ctr,
                                                       key, nonce);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(key, sizeof(key));
        ceepew_secure_zero(nonce, sizeof(nonce));
        return err;
    }

    uint16_t pt_len = GATT_PLAINTEXT_BYTES;
    err = crypto_ascon_aead_decrypt(key, nonce,
                                    &wire_ctr, GATT_COUNTER_BYTES,
                                    ct_tag, GATT_CRYPTO_TOTAL_BYTES - GATT_COUNTER_BYTES,
                                    plaintext_out, &pt_len);
    ceepew_secure_zero(key, sizeof(key));
    ceepew_secure_zero(nonce, sizeof(nonce));
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

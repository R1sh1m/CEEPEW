/* components/crypto/crypto_eddsa.c
 *
 * Ed25519 sign / verify / keygen backed by the ESP-IDF mbedTLS
 * PSA Crypto API (tf-psa-crypto).
 *
 * Replaces a 395-line hand-rolled TweetNaCl port. The PSA path uses
 * PSA_ALG_PURE_EDDSA, which is RFC 8032 deterministic Ed25519.
 *
 * Public API is unchanged so the rest of the codebase does not need
 * to know we switched backends:
 *
 *   CeePewErr_t crypto_eddsa_keypair(uint8_t pk[32], uint8_t sk[64]);
 *   CeePewErr_t crypto_eddsa_seeded_keypair(uint8_t pk[32],
 *                                           uint8_t sk[64],
 *                                           const uint8_t seed[32]);
 *   CeePewErr_t crypto_eddsa_sign(const uint8_t priv[64],
 *                                 const uint8_t *msg, uint16_t msg_len,
 *                                 uint8_t sig[64]);
 *   CeePewErr_t crypto_eddsa_verify(const uint8_t pub[32],
 *                                   const uint8_t *msg, uint16_t msg_len,
 *                                   const uint8_t sig[64]);
 *
 * sk[] layout: sk[0..32) = seed, sk[32..64) = public key.
 * priv[] layout (for sign): same as sk.
 *
 * The PSA key store holds the canonical 32-byte Ed25519 seed; the
 * 64-byte sk/priv layout is preserved at this API boundary because
 * the rest of the project (esp_test_signing, handoff in BLE, on-wire
 * packets) encodes the keypair that way. The duplicate public-key
 * bytes in priv[] are ignored on the sign path — the PSA key is the
 * source of truth and we re-derive the public key from the seed.
 *
 * All key handles are PSA_KEY_LIFETIME_VOLATILE and are destroyed
 * before each function returns, so no long-lived key state lives
 * in PSA's key store between calls.
 */

#include "crypto_eddsa.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_security_utils.h"
#include "crypto_rng.h"

#include <stdint.h>
#include <string.h>

#include "psa/crypto.h"

/* Set PSA algorithm / type attributes for a 32-byte Ed25519 seed. */
static void ed25519_seed_attrs(psa_key_attributes_t *attrs)
{
    (void)psa_set_key_type(attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_TWISTED_EDWARDS));
    (void)psa_set_key_bits(attrs, 255U);
    (void)psa_set_key_algorithm(attrs, PSA_ALG_PURE_EDDSA);
    (void)psa_set_key_usage_flags(attrs,
                                  PSA_KEY_USAGE_SIGN_MESSAGE |
                                  PSA_KEY_USAGE_EXPORT |
                                  PSA_KEY_USAGE_COPY);
}

/* Set PSA attributes for a 32-byte Ed25519 public key (for verify). */
static void ed25519_pub_attrs(psa_key_attributes_t *attrs)
{
    (void)psa_set_key_type(attrs, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_TWISTED_EDWARDS));
    (void)psa_set_key_bits(attrs, 255U);
    (void)psa_set_key_algorithm(attrs, PSA_ALG_PURE_EDDSA);
    (void)psa_set_key_usage_flags(attrs, PSA_KEY_USAGE_VERIFY_MESSAGE);
}

/* Shared seed→keypair routine. Returns CEEPEW_OK on success. On any
 * PSA error, the key (if allocated) is destroyed before returning. */
static CeePewErr_t ed25519_seed_to_keypair(const uint8_t seed[32],
                                           uint8_t pk[32],
                                           uint8_t sk_full[64])
{
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    uint8_t pub_buf[32] = {0};
    size_t pub_len = 0;
    psa_status_t status;

    ed25519_seed_attrs(&attrs);

    /* Import the 32-byte seed. PSA stores it as an Ed25519 keypair,
     * deriving the public key internally. */
    status = psa_import_key(&attrs, seed, 32U, &key_id);
    if (status != PSA_SUCCESS) {
        (void)psa_reset_key_attributes(&attrs);
        return CEEPEW_ERR_CRYPTO;
    }

    /* Export the derived public key. */
    status = psa_export_public_key(key_id, pub_buf, sizeof(pub_buf), &pub_len);
    if (status != PSA_SUCCESS || pub_len != 32U) {
        (void)psa_destroy_key(key_id);
        (void)psa_reset_key_attributes(&attrs);
        ceepew_secure_zero(pub_buf, sizeof(pub_buf));
        return CEEPEW_ERR_CRYPTO;
    }

    /* Copy public key out, then destroy the PSA key (we only needed
     * it long enough to derive pk). The caller owns seed and pk. */
    (void)memcpy(pk, pub_buf, 32U);
    (void)memcpy(sk_full, seed, 32U);
    (void)memcpy(&sk_full[32], pub_buf, 32U);

    (void)psa_destroy_key(key_id);
    (void)psa_reset_key_attributes(&attrs);
    ceepew_secure_zero(pub_buf, sizeof(pub_buf));
    return CEEPEW_OK;
}

CeePewErr_t crypto_eddsa_keypair(uint8_t pk[32], uint8_t sk[64])
{
    CEEPEW_ASSERT(pk != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(sk != NULL, CEEPEW_ERR_NULL_PTR);

    CeePewErr_t herr = crypto_rng_health_check();
    if (herr != CEEPEW_OK) { return herr; }

    uint8_t seed[32] = {0};
    CeePewErr_t err = crypto_rng_fill(seed, sizeof(seed));
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(seed, sizeof(seed));
        return err;
    }

    CeePewErr_t kerr = ed25519_seed_to_keypair(seed, pk, sk);
    ceepew_secure_zero(seed, sizeof(seed));
    return kerr;
}

CeePewErr_t crypto_eddsa_seeded_keypair(uint8_t pk[32], uint8_t sk[64],
                                        const uint8_t seed[32])
{
    CEEPEW_ASSERT(pk != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(sk != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(seed != NULL, CEEPEW_ERR_NULL_PTR);
    return ed25519_seed_to_keypair(seed, pk, sk);
}

CeePewErr_t crypto_eddsa_sign(const uint8_t priv[64],
                              const uint8_t *msg, uint16_t msg_len,
                              uint8_t sig[64])
{
    CEEPEW_ASSERT(priv != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg != NULL || msg_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(sig != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg_len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    uint8_t sig_buf[64] = {0};
    size_t sig_len = 0;
    psa_status_t status;
    CeePewErr_t result = CEEPEW_OK;

    ed25519_seed_attrs(&attrs);

    /* Import the 32-byte seed (first half of priv). The 32-byte
     * public-key half of priv[] is ignored — PSA derives the public
     * key from the seed. */
    status = psa_import_key(&attrs, priv, 32U, &key_id);
    if (status != PSA_SUCCESS) {
        (void)psa_reset_key_attributes(&attrs);
        return CEEPEW_ERR_CRYPTO;
    }

    status = psa_sign_message(key_id, PSA_ALG_PURE_EDDSA,
                              msg, (size_t)msg_len,
                              sig_buf, sizeof(sig_buf), &sig_len);
    if (status != PSA_SUCCESS || sig_len != 64U) {
        result = CEEPEW_ERR_CRYPTO;
    } else {
        (void)memcpy(sig, sig_buf, 64U);
    }

    (void)psa_destroy_key(key_id);
    (void)psa_reset_key_attributes(&attrs);
    ceepew_secure_zero(sig_buf, sizeof(sig_buf));
    return result;
}

CeePewErr_t crypto_eddsa_verify(const uint8_t pub[32],
                                const uint8_t *msg, uint16_t msg_len,
                                const uint8_t sig[64])
{
    CEEPEW_ASSERT(pub != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg != NULL || msg_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(sig != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg_len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    psa_status_t status;

    ed25519_pub_attrs(&attrs);

    status = psa_import_key(&attrs, pub, 32U, &key_id);
    if (status != PSA_SUCCESS) {
        (void)psa_reset_key_attributes(&attrs);
        return CEEPEW_ERR_CRYPTO;
    }

    status = psa_verify_message(key_id, PSA_ALG_PURE_EDDSA,
                                msg, (size_t)msg_len,
                                sig, 64U);

    (void)psa_destroy_key(key_id);
    (void)psa_reset_key_attributes(&attrs);

    if (status == PSA_SUCCESS) { return CEEPEW_OK; }
    if (status == PSA_ERROR_INVALID_SIGNATURE) { return CEEPEW_ERR_SIG_FAIL; }
    return CEEPEW_ERR_CRYPTO;
}

/* components/crypto/crypto_pake.c
 *
 * CEE-PEW Password-Authenticated Key Exchange (PAKE) Engine.
 *
 * Zero-heap, side-channel-hardened balanced PAKE over Curve25519.
 */

#include "crypto_pake.h"
#include "curve25519.h"
#include "crypto_ecdh.h"
#include "crypto_hkdf.h"
#include "crypto_hmac.h"
#include "crypto_rng.h"
#include "ceepew_assert.h"
#include "ceepew_security_utils.h"
#include <string.h>

static const uint8_t s_cofactor_8[32U] = {
    8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U
};

static const char *PAKE_INFO_G = "CEEPEW-PAKE-G-GEN-V1";
static const char *PAKE_INFO_SK = "ceepew-pake-sk";
static const char *PAKE_INFO_INIT_MAC = "ceepew-pake-init-mac";
static const char *PAKE_INFO_RESP_MAC = "ceepew-pake-resp-mac";

CeePewErr_t crypto_pake_init(const char *code, uint16_t code_len,
                             const uint8_t salt[CEEPEW_PAKE_SALT_BYTES],
                             uint8_t local_priv[32U],
                             uint8_t local_pub[CEEPEW_PAKE_PUBKEY_BYTES])
{
    CEEPEW_ASSERT(code != NULL && code_len > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(salt != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(local_priv != NULL && local_pub != NULL, CEEPEW_ERR_NULL_PTR);

    /* 1. Derive candidate generator seed from code and salt via HKDF-Extract + Expand */
    uint8_t prk[32U];
    CeePewErr_t err = crypto_hmac_sha256(salt, CEEPEW_PAKE_SALT_BYTES,
                                         (const uint8_t *)code, (uint32_t)code_len,
                                         prk);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(prk, sizeof(prk));
        return err;
    }

    uint8_t raw_u[32U];
    err = crypto_hkdf_expand(prk, (const uint8_t *)PAKE_INFO_G,
                             (uint8_t)strlen(PAKE_INFO_G),
                             raw_u, (uint8_t)sizeof(raw_u));
    ceepew_secure_zero(prk, sizeof(prk));
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(raw_u, sizeof(raw_u));
        return err;
    }

    /* Mask high bit to ensure canonical 255-bit field element */
    raw_u[31U] &= 0x7FU;

    /* Multiply by cofactor 8 to clear any low-order torsion component */
    uint8_t gen_point[32U];
    int rc = curve25519_scalarmult(gen_point, s_cofactor_8, raw_u);
    ceepew_secure_zero(raw_u, sizeof(raw_u));
    if (rc != 0 || ceepew_is_all_zero(gen_point, sizeof(gen_point))) {
        /* Fallback: if raw_u was in small subgroup, use base point scalar mult */
        uint8_t fallback_scalar[32U];
        memcpy(fallback_scalar, s_cofactor_8, sizeof(fallback_scalar));
        curve25519_clamp(fallback_scalar);
        rc = curve25519_scalarmult_base(gen_point, fallback_scalar);
        ceepew_secure_zero(fallback_scalar, sizeof(fallback_scalar));
        if (rc != 0) {
            ceepew_secure_zero(gen_point, sizeof(gen_point));
            return CEEPEW_ERR_CRYPTO;
        }
    }

    /* 2. Generate ephemeral scalar */
    err = crypto_rng_fill(local_priv, 32U);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(gen_point, sizeof(gen_point));
        ceepew_secure_zero(local_priv, 32U);
        return err;
    }
    curve25519_clamp(local_priv);

    /* 3. Compute public point: local_pub = local_priv * G */
    rc = curve25519_scalarmult(local_pub, local_priv, gen_point);
    ceepew_secure_zero(gen_point, sizeof(gen_point));
    if (rc != 0 || ceepew_is_all_zero(local_pub, CEEPEW_PAKE_PUBKEY_BYTES)) {
        ceepew_secure_zero(local_priv, 32U);
        ceepew_secure_zero(local_pub, CEEPEW_PAKE_PUBKEY_BYTES);
        return CEEPEW_ERR_CRYPTO;
    }

    return CEEPEW_OK;
}

CeePewErr_t crypto_pake_compute(const uint8_t local_priv[32U],
                                const uint8_t peer_pub[CEEPEW_PAKE_PUBKEY_BYTES],
                                const uint8_t *transcript, uint16_t transcript_len,
                                bool is_initiator,
                                uint8_t session_key_out[CEEPEW_PAKE_KEY_BYTES],
                                uint8_t local_confirm_out[CEEPEW_PAKE_CONFIRM_BYTES],
                                uint8_t peer_confirm_expected[CEEPEW_PAKE_CONFIRM_BYTES])
{
    CEEPEW_ASSERT(local_priv != NULL && peer_pub != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(session_key_out != NULL && local_confirm_out != NULL && peer_confirm_expected != NULL,
                  CEEPEW_ERR_NULL_PTR);

    /* RFC 7748 check: reject low-order public keys */
    CeePewErr_t err = crypto_ecdh_reject_low_order(peer_pub);
    if (err != CEEPEW_OK) {
        return CEEPEW_ERR_CRYPTO;
    }

    /* Compute K = local_priv * peer_pub */
    uint8_t k_shared[32U];
    int rc = curve25519_scalarmult(k_shared, local_priv, peer_pub);
    if (rc != 0 || ceepew_is_all_zero(k_shared, sizeof(k_shared))) {
        ceepew_secure_zero(k_shared, sizeof(k_shared));
        return CEEPEW_ERR_CRYPTO;
    }

    /* Derive master PRK binding transcript via HKDF-Extract: PRK = HMAC(transcript, K) */
    uint8_t prk[32U];
    err = crypto_hmac_sha256(transcript, transcript_len, k_shared, (uint32_t)sizeof(k_shared), prk);
    ceepew_secure_zero(k_shared, sizeof(k_shared));
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(prk, sizeof(prk));
        return err;
    }

    /* Derive session key */
    err = crypto_hkdf_expand(prk, (const uint8_t *)PAKE_INFO_SK,
                             (uint8_t)strlen(PAKE_INFO_SK),
                             session_key_out, (uint8_t)CEEPEW_PAKE_KEY_BYTES);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(prk, sizeof(prk));
        ceepew_secure_zero(session_key_out, CEEPEW_PAKE_KEY_BYTES);
        return err;
    }

    /* Derive confirmation MACs */
    uint8_t init_mac[CEEPEW_PAKE_CONFIRM_BYTES];
    uint8_t resp_mac[CEEPEW_PAKE_CONFIRM_BYTES];

    err = crypto_hkdf_expand(prk, (const uint8_t *)PAKE_INFO_INIT_MAC,
                             (uint8_t)strlen(PAKE_INFO_INIT_MAC),
                             init_mac, (uint8_t)sizeof(init_mac));
    if (err == CEEPEW_OK) {
        err = crypto_hkdf_expand(prk, (const uint8_t *)PAKE_INFO_RESP_MAC,
                                 (uint8_t)strlen(PAKE_INFO_RESP_MAC),
                                 resp_mac, (uint8_t)sizeof(resp_mac));
    }
    ceepew_secure_zero(prk, sizeof(prk));

    if (err != CEEPEW_OK) {
        ceepew_secure_zero(init_mac, sizeof(init_mac));
        ceepew_secure_zero(resp_mac, sizeof(resp_mac));
        ceepew_secure_zero(session_key_out, CEEPEW_PAKE_KEY_BYTES);
        return err;
    }

    if (is_initiator) {
        memcpy(local_confirm_out, init_mac, CEEPEW_PAKE_CONFIRM_BYTES);
        memcpy(peer_confirm_expected, resp_mac, CEEPEW_PAKE_CONFIRM_BYTES);
    } else {
        memcpy(local_confirm_out, resp_mac, CEEPEW_PAKE_CONFIRM_BYTES);
        memcpy(peer_confirm_expected, init_mac, CEEPEW_PAKE_CONFIRM_BYTES);
    }

    ceepew_secure_zero(init_mac, sizeof(init_mac));
    ceepew_secure_zero(resp_mac, sizeof(resp_mac));
    return CEEPEW_OK;
}

CeePewErr_t crypto_pake_verify_confirm(const uint8_t received_confirm[CEEPEW_PAKE_CONFIRM_BYTES],
                                       const uint8_t expected_confirm[CEEPEW_PAKE_CONFIRM_BYTES])
{
    CEEPEW_ASSERT(received_confirm != NULL && expected_confirm != NULL, CEEPEW_ERR_NULL_PTR);
    if (!ceepew_ct_equal(received_confirm, expected_confirm, CEEPEW_PAKE_CONFIRM_BYTES)) {
        return CEEPEW_ERR_CRYPTO;
    }
    return CEEPEW_OK;
}

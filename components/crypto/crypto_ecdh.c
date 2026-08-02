/* components/crypto/crypto_ecdh.c */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_security_utils.h"
#include "crypto_rng.h"
#include "curve25519.h"

#include <stdint.h>
#include <string.h>

/* ── Low-order point rejection (RFC 7748 §6.1) ────────────────────────
 *
 * The 7 byte arrays below represent all Curve25519 u-coordinates whose
 * corresponding point has order dividing 8 (the curve cofactor).  When
 * clamped to a multiple of 8 the scalar always produces the all-zero
 * shared secret from these points, but pre-checking prevents a wasted
 * scalar multiplication and provides the stronger RFC 7748 guarantee.
 *
 * Source: libsodium's x25519_ref10.c blocklist (7 entries correspond to
 * the 8 torsion points; the identity has no u-coordinate representation).
 */
static const uint8_t s_low_order_points[7][32] = {
    /*   0 (u=0, non-canonical p ≡ 0)              */
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /*   1 (u=1)                                   */
    { 0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /*   32560625091655743179598362635611063...504   (order 8)          */
    { 0xe0,0xeb,0x7a,0x7c,0x3b,0x41,0xb8,0xae,0x16,0x56,0xe3,0xfa,0xf1,0x9f,0xc4,0x6a,
      0xda,0x09,0x8d,0xeb,0x9c,0x32,0xb1,0xfd,0x86,0x62,0x05,0x16,0x5f,0x49,0xb8,0x00 },
    /*   39382357235489614581723060781553021...823   (order 8)          */
    { 0x5f,0x9c,0x95,0xbc,0xa3,0x50,0x8c,0x24,0xb1,0xd0,0xb1,0x55,0x9c,0x83,0xef,0x5b,
      0x04,0x44,0x5c,0xc4,0x58,0x1c,0x8e,0x86,0xd8,0x22,0x4e,0xdd,0xd0,0x9f,0x11,0x57 },
    /*   p-1 (order 2)                              */
    { 0xec,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f },
    /*   p (≡ 0 mod p, order 4)                     */
    { 0xed,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f },
    /*   p+1 (≡ 1 mod p, order 1)                   */
    { 0xee,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f },
};

/* Reject peer public keys that belong to the 8-torsion subgroup.
 * Returns CEEPEW_ERR_CRYPTO if pub matches one of the known low-order
 * u-coordinate encodings, CEEPEW_OK otherwise.  Constant-time. */
CeePewErr_t crypto_ecdh_reject_low_order(const uint8_t pub[32])
{
    uint8_t overall = 0U;
    for (uint32_t j = 0U; j < 7U; j++) {
        uint8_t acc = 0U;
        for (uint32_t i = 0U; i < 32U; i++) {
            acc |= (uint8_t)(pub[i] ^ s_low_order_points[j][i]);
        }
        overall |= (uint8_t)(acc == 0U ? 1U : 0U);
    }
    return (overall != 0U) ? CEEPEW_ERR_CRYPTO : CEEPEW_OK;
}

CeePewErr_t crypto_ecdh_shared_secret(const uint8_t priv[32],
                                       const uint8_t pub[32],
                                       uint8_t ss[32])
{
    CEEPEW_ASSERT(priv != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(pub != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(ss != NULL, CEEPEW_ERR_NULL_PTR);

    /* RFC 7748 §6.1: reject known low-order public keys */
    CeePewErr_t err = crypto_ecdh_reject_low_order(pub);
    if (err != CEEPEW_OK) {
        return CEEPEW_ERR_CRYPTO;
    }

    uint8_t scalar[32U];
    memcpy(scalar, priv, sizeof(scalar));
    curve25519_clamp(scalar);
    int rc = curve25519_scalarmult(ss, scalar, pub);
    if (rc != 0) {
        ceepew_secure_zero(scalar, (uint32_t)sizeof(scalar));
        ceepew_secure_zero(ss, 32U);
        return CEEPEW_ERR_CRYPTO;
    }

    /* RFC 7748 §6: reject all-zero shared secret (small-subgroup output) */
    if (ceepew_is_all_zero(ss, 32U)) {
        ceepew_secure_zero(scalar, (uint32_t)sizeof(scalar));
        ceepew_secure_zero(ss, 32U);
        return CEEPEW_ERR_CRYPTO;
    }

    ceepew_secure_zero(scalar, (uint32_t)sizeof(scalar));
    return CEEPEW_OK;
}

CeePewErr_t crypto_ecdh_generate_keypair(uint8_t pub[32], uint8_t priv[32])
{
    CEEPEW_ASSERT(pub != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(priv != NULL, CEEPEW_ERR_NULL_PTR);

    CeePewErr_t err = crypto_rng_fill(priv, 32U);
    if (err != CEEPEW_OK) {
        return err;
    }

    curve25519_clamp(priv);
    int rc = curve25519_scalarmult_base(pub, priv);
    if (rc != 0) {
        ceepew_secure_zero(priv, 32U);
        return CEEPEW_ERR_CRYPTO;
    }
    return CEEPEW_OK;
}

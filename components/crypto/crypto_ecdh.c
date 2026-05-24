/* components/crypto/crypto_ecdh.c */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_security_utils.h"
#include "crypto_rng.h"
#include "curve25519.h"

#include <stdint.h>
#include <string.h>

CeePewErr_t crypto_ecdh_shared_secret(const uint8_t priv[32],
                                       const uint8_t pub[32],
                                       uint8_t ss[32])
{
    CEEPEW_ASSERT(priv != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(pub != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(ss != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t scalar[32U];
    memcpy(scalar, priv, sizeof(scalar));
    curve25519_clamp(scalar);
    int rc = curve25519_scalarmult(ss, scalar, pub);
    ceepew_secure_zero(scalar, (uint32_t)sizeof(scalar));
    return (rc == 0) ? CEEPEW_OK : CEEPEW_ERR_CRYPTO;
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

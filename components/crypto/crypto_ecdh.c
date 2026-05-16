/* components/crypto/crypto_ecdh.c */

#include "crypto_hkdf.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include "crypto_rng.h"
#include <stdint.h>
#include <string.h>

/*
 * Curve25519 (X25519) ECDH helpers.
 * Implementation note: this file provides a stable wrapper API. An audited
 * upstream implementation (TweetNaCl or libsodium) should be linked during
 * build. For now the functions return CEEPEW_ERR_UNSUPPORTED until the
 * upstream binding is added.
 */

/* SECURITY: ECDH shared secret must be mixed through HKDF before use.
 * (threat: raw ECDH secret leakage)
 */
CeePewErr_t crypto_ecdh_shared_secret(const uint8_t priv[32], const uint8_t pub[32], uint8_t ss[32]){
    CEEPEW_ASSERT(priv != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(pub != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(ss != NULL, CEEPEW_ERR_NULL_PTR);

    /* Upstream integration placeholder: perform X25519(priv, pub) -> ss */

    return CEEPEW_ERR_UNSUPPORTED;
}

CeePewErr_t crypto_ecdh_generate_keypair(uint8_t pub[32], uint8_t priv[32]){
    CEEPEW_ASSERT(pub != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(priv != NULL, CEEPEW_ERR_NULL_PTR);

    /* Generate 32 bytes of entropy for the private scalar. Caller must
     * clamp per X25519 spec when upstream implementation is added.
     */
    CeePewErr_t err = crypto_rng_fill(priv, 32U);
    if (err != CEEPEW_OK){return err;}
    
    /* Upstream integration placeholder: compute public key from priv -> pub */

    return CEEPEW_ERR_UNSUPPORTED;
}

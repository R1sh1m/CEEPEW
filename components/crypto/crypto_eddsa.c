/* components/crypto/crypto_eddsa.c */

#include "crypto_hkdf.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include <stdint.h>
#include <string.h>

/* Ed25519 signing/verification wrappers.
 * Integrate a well-audited library (TweetNaCl/Ed25519) and replace the
 * CEEPEW_ERR_UNSUPPORTED returns with calls into that library.
 */

CeePewErr_t crypto_eddsa_sign(const uint8_t priv[64], const uint8_t *msg, uint16_t msg_len, uint8_t sig[64]){
    CEEPEW_ASSERT(priv != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg != NULL || msg_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(sig != NULL, CEEPEW_ERR_NULL_PTR);

    /* Placeholder: call into Ed25519 signing implementation */
    (void)priv; (void)msg; (void)msg_len; (void)sig;
    return CEEPEW_ERR_UNSUPPORTED;
}

CeePewErr_t crypto_eddsa_verify(const uint8_t pub[32], const uint8_t *msg, uint16_t msg_len, const uint8_t sig[64]){
    CEEPEW_ASSERT(pub != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg != NULL || msg_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(sig != NULL, CEEPEW_ERR_NULL_PTR);

    /* Placeholder: call into Ed25519 verify implementation */
    (void)pub; (void)msg; (void)msg_len; (void)sig;
    return CEEPEW_ERR_UNSUPPORTED;
}

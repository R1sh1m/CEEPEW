/* components/crypto/crypto_ascon.c */
#include "crypto_hkdf.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include <stdint.h>
#include <string.h>
/* Ascon-128 AEAD outer-envelope wrapper.
 * This wrapper will invoke an audited Ascon implementation when vendored.
 * For now functions return CEEPEW_ERR_UNSUPPORTED to indicate integration
 * is pending.
 */

CeePewErr_t crypto_ascon_aead_encrypt(const uint8_t key[16], const uint8_t nonce[16], const uint8_t *ad, uint16_t ad_len, const uint8_t *pt, uint16_t pt_len, uint8_t *ct, uint16_t *ct_len){
    CEEPEW_ASSERT(key != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(nonce != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(ct != NULL && ct_len != NULL, CEEPEW_ERR_NULL_PTR);
    /* Placeholder for Ascon-128 AEAD encrypt */
    (void)key; (void)nonce; (void)ad; (void)ad_len; (void)pt; (void)pt_len; (void)ct; (void)ct_len;
    return CEEPEW_ERR_UNSUPPORTED;
}
CeePewErr_t crypto_ascon_aead_decrypt(const uint8_t key[16], const uint8_t nonce[16], const uint8_t *ad, uint16_t ad_len, const uint8_t *ct, uint16_t ct_len, uint8_t *pt, uint16_t *pt_len){
    CEEPEW_ASSERT(key != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(nonce != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(pt != NULL && pt_len != NULL, CEEPEW_ERR_NULL_PTR);
    /* Placeholder for Ascon-128 AEAD decrypt */
    (void)key; (void)nonce; (void)ad; (void)ad_len; (void)ct; (void)ct_len; (void)pt; (void)pt_len;
    return CEEPEW_ERR_UNSUPPORTED;
}

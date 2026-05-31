/* components/crypto/crypto_ascon.h */

#ifndef CRYPTO_ASCON_H
#define CRYPTO_ASCON_H

#include "ceepew_assert.h"
#include <stdint.h>

/* Ascon-128 AEAD encryption
 *
 * PARAMETERS:
 *   key      - 16-byte session key
 *   nonce    - 16-byte nonce
 *   ad       - Associated data (header/MAC); NULL if no AD
 *   ad_len   - Length of AD (0 if NULL)
 *   pt       - Plaintext (can be NULL if pt_len = 0)
 *   pt_len   - Plaintext length
 *   ct       - Output ciphertext buffer (must be >= pt_len + 16)
 *   ct_len   - [OUT] Length of ciphertext (pt_len + 16)
 *
 * RETURNS:
 *   CEEPEW_OK on success; error code otherwise.
 */
CeePewErr_t crypto_ascon_aead_encrypt(const uint8_t key[16],
                                      const uint8_t nonce[16],
                                      const uint8_t *ad,
                                      uint16_t ad_len,
                                      const uint8_t *pt,
                                      uint16_t pt_len,
                                      uint8_t *ct,
                                      uint16_t *ct_len);

/* Ascon-128 AEAD decryption
 *
 * PARAMETERS:
 *   key      - 16-byte session key
 *   nonce    - 16-byte nonce
 *   ad       - Associated data (header/MAC); NULL if no AD
 *   ad_len   - Length of AD (0 if NULL)
 *   ct       - Ciphertext (includes 16-byte tag at end)
 *   ct_len   - Length of ciphertext (>= 16 for tag)
 *   pt       - Output plaintext buffer (must be >= ct_len - 16)
 *   pt_len   - [OUT] Length of plaintext (ct_len - 16)
 *
 * RETURNS:
 *   CEEPEW_OK on success (tag verified); CEEPEW_ERR_CRYPTO on tag mismatch (silent fail).
 *   Returns error code for other failures (bounds, null pointer, etc).
 */
CeePewErr_t crypto_ascon_aead_decrypt(const uint8_t key[16],
                                      const uint8_t nonce[16],
                                      const uint8_t *ad,
                                      uint16_t ad_len,
                                      const uint8_t *ct,
                                      uint16_t ct_len,
                                      uint8_t *pt,
                                      uint16_t *pt_len);

#endif /* CRYPTO_ASCON_H */

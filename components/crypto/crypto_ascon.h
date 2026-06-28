/* components/crypto/crypto_ascon.h */

#ifndef CRYPTO_ASCON_H
#define CRYPTO_ASCON_H

#include "ceepew_assert.h"
#include <stdint.h>

/* Ascon-128 AEAD encrypt. ct must have space for pt_len + 16 tag bytes. */
CeePewErr_t crypto_ascon_aead_encrypt(const uint8_t key[16],
                                      const uint8_t nonce[16],
                                      const uint8_t *ad,
                                      uint16_t ad_len,
                                      const uint8_t *pt,
                                      uint16_t pt_len,
                                      uint8_t *ct,
                                      uint16_t *ct_len);

/* Ascon-128 AEAD decrypt. Returns CEEPEW_ERR_CRYPTO on tag mismatch (silent). */
CeePewErr_t crypto_ascon_aead_decrypt(const uint8_t key[16],
                                      const uint8_t nonce[16],
                                      const uint8_t *ad,
                                      uint16_t ad_len,
                                      const uint8_t *ct,
                                      uint16_t ct_len,
                                      uint8_t *pt,
                                      uint16_t *pt_len);

#endif /* CRYPTO_ASCON_H */

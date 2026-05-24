/* components/crypto/crypto_hkdf.h */

#ifndef CRYPTO_HKDF_H
#define CRYPTO_HKDF_H

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <stdint.h>

CeePewErr_t crypto_hkdf_derive(const uint8_t *ikm, uint8_t ikm_len, const uint8_t *salt, uint8_t salt_len, const uint8_t *info, uint8_t info_len, uint8_t *out, uint8_t out_len);

#endif /* CRYPTO_HKDF_H */

/* components/crypto/crypto_sha256.h */

#ifndef CRYPTO_SHA256_H
#define CRYPTO_SHA256_H

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <stdint.h>

CeePewErr_t crypto_sha256_compute(const uint8_t *in, uint32_t len, uint8_t out[32]);

#endif /* CRYPTO_SHA256_H */

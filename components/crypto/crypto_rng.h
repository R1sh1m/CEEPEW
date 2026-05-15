/* components/crypto/crypto_rng.h */

#ifndef CRYPTO_RNG_H
#define CRYPTO_RNG_H

#include "../../main/ceepew_config.h"
#include "../../main/ceepew_assert.h"
#include <stdint.h>

CeePewErr_t crypto_rng_fill(uint8_t *buf, uint32_t len);

#endif /* CRYPTO_RNG_H */

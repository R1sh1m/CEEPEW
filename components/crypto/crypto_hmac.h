#ifndef CRYPTO_HMAC_H
#define CRYPTO_HMAC_H

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <stdint.h>

CeePewErr_t crypto_hmac_sha256(const uint8_t *key, uint16_t key_len,
                               const uint8_t *msg, uint32_t msg_len,
                               uint8_t out[32]);

#endif /* CRYPTO_HMAC_H */
/* components/crypto/crypto_pad.h */

#ifndef CRYPTO_PAD_H
#define CRYPTO_PAD_H

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <stdint.h>

#define CEEPEW_PAD_BLOCK_SIZE 64U

/**
 * @brief Apply constant-time PKCS#7 padding to align input to 64-byte blocks.
 *
 * Appends 1 to 64 padding bytes according to PKCS#7 specification.
 * If input is already an exact multiple of 64 bytes, a full 64-byte block is added.
 */
CeePewErr_t crypto_pad_apply(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len);

/**
 * @brief Verify and strip PKCS#7 padding in constant time.
 *
 * Verifies that the padding bytes are uncorrupted without timing leakage.
 */
CeePewErr_t crypto_pad_remove(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len);

#endif /* CRYPTO_PAD_H */

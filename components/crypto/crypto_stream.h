/* components/crypto/crypto_stream.h */

#ifndef CRYPTO_STREAM_H
#define CRYPTO_STREAM_H

#include "ceepew_assert.h"
#include "ceepew_config.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  subkey[32U];
    uint8_t  nonce_tail[8U];
    uint8_t  block[64U];
    uint64_t block_counter;
    uint8_t  block_used;
    bool     active;
} StreamCipher_t;

CeePewErr_t crypto_stream_init(StreamCipher_t *ctx,
                               const uint8_t key[32U],
                               const uint8_t nonce[24U]);
CeePewErr_t crypto_stream_process(StreamCipher_t *ctx,
                                 const uint8_t *in,
                                 uint16_t in_len,
                                 uint8_t *out,
                                 uint16_t *out_len);
CeePewErr_t crypto_stream_finalise(StreamCipher_t *ctx);

#endif /* CRYPTO_STREAM_H */

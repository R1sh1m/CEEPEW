/* components/crypto/crypto_box_wrap.h */

#ifndef CRYPTO_BOX_WRAP_H
#define CRYPTO_BOX_WRAP_H

#include "crypto_ctx.h"

#include <stdint.h>

#define CRYPTO_BOX_ZEROBYTES     32U
#define CRYPTO_BOX_BOXZEROBYTES  16U
#define CRYPTO_BOX_NONCEBYTES    24U

CeePewErr_t crypto_box_encrypt(CryptoCtx_t *ctx,
                               const uint8_t peer_public_key[32U],
                               const uint8_t *msg,
                               uint16_t msg_len,
                               uint8_t *out,
                               uint16_t *out_len);

CeePewErr_t crypto_box_decrypt(const CryptoCtx_t *ctx,
                               const uint8_t nonce[CRYPTO_BOX_NONCEBYTES],
                               const uint8_t peer_public_key[32U],
                               const uint8_t *in,
                               uint16_t in_len,
                               uint8_t *out,
                               uint16_t *out_len);

#endif /* CRYPTO_BOX_WRAP_H */

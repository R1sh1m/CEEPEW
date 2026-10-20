/* components/crypto/crypto_box_wrap.h */

#ifndef CRYPTO_BOX_WRAP_H
#define CRYPTO_BOX_WRAP_H

#include "crypto_ctx.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"

#include <stdint.h>

#define CRYPTO_BOX_ZEROBYTES       32U
#define CRYPTO_BOX_BOXZEROBYTES    16U
#define CRYPTO_BOX_NONCEBYTES      24U
#define CRYPTO_BOX_INNER_MAX_BYTES (CEEPEW_MAX_MSG_BYTES + CEEPEW_ASCON_TAG_BYTES)

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

/* Derive and cache the X25519 shared secret + stream key for the peer key
 * already stored in ctx->peer_box_pubkey. Call once after
 * session_set_peer_box_pubkey(); encrypt/decrypt reuse the cache.
 * Sets ctx->box_shared_valid on success. */
CeePewErr_t crypto_box_refresh_cache(CryptoCtx_t *ctx);

/* Securely invalidate the cached shared secret + stream key
 * (ceepew_secure_zero + box_shared_valid=false). Call on session
 * teardown and PFS key rotation. */
CeePewErr_t crypto_box_invalidate_cache(CryptoCtx_t *ctx);

#endif /* CRYPTO_BOX_WRAP_H */

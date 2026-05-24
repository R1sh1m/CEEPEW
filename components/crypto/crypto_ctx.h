/* components/crypto/crypto_ctx.h */

#ifndef CRYPTO_CTX_H
#define CRYPTO_CTX_H

#include "ceepew_assert.h"
#include "ceepew_config.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     session_active;
    uint64_t nonce_counter;
    uint8_t  ascon_key[CEEPEW_SESSION_KEY_BYTES];
    uint8_t  box_seed[32U];
    uint8_t  session_id[8U];
    uint8_t  reserved[8U];
} CryptoCtx_t;

extern CryptoCtx_t g_crypto_ctx;

CeePewErr_t crypto_ctx_init(void);
CeePewErr_t crypto_ctx_destroy(void);

#endif /* CRYPTO_CTX_H */

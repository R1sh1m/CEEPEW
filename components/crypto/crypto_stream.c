/* components/crypto/crypto_stream.c */

#include "crypto_stream.h"
#include "ceepew_security_utils.h"

#include <string.h>

static const uint8_t S20_SIGMA[17U] = "expand 32-byte k";

static uint32_t load32_le(const uint8_t *in)
{
    return ((uint32_t)in[0]) |
           ((uint32_t)in[1] << 8U) |
           ((uint32_t)in[2] << 16U) |
           ((uint32_t)in[3] << 24U);
}

static void store32_le(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v);
    out[1] = (uint8_t)(v >> 8U);
    out[2] = (uint8_t)(v >> 16U);
    out[3] = (uint8_t)(v >> 24U);
}

static uint32_t rotl32(uint32_t v, uint8_t n)
{
    return (v << n) | (v >> (32U - n));
}

static void salsa20_core(uint8_t out[64U],
                         const uint8_t key[32U],
                         const uint8_t nonce[8U],
                         uint64_t counter)
{
    uint32_t x[16];
    uint32_t z[16];

    x[0]  = load32_le(&S20_SIGMA[0]);
    x[5]  = load32_le(&S20_SIGMA[4]);
    x[10] = load32_le(&S20_SIGMA[8]);
    x[15] = load32_le(&S20_SIGMA[12]);
    x[1]  = load32_le(&key[0]);
    x[2]  = load32_le(&key[4]);
    x[3]  = load32_le(&key[8]);
    x[4]  = load32_le(&key[12]);
    x[11] = load32_le(&key[16]);
    x[12] = load32_le(&key[20]);
    x[13] = load32_le(&key[24]);
    x[14] = load32_le(&key[28]);
    x[6]  = load32_le(&nonce[0]);
    x[7]  = load32_le(&nonce[4]);
    x[8]  = (uint32_t)counter;
    x[9]  = (uint32_t)(counter >> 32U);

    for (uint8_t i = 0U; i < 16U; i++) {
        z[i] = x[i];
    }

    for (uint8_t r = 0U; r < 10U; r++) {
        z[4] ^= rotl32(z[0] + z[12], 7U);
        z[8] ^= rotl32(z[4] + z[0], 9U);
        z[12] ^= rotl32(z[8] + z[4], 13U);
        z[0] ^= rotl32(z[12] + z[8], 18U);

        z[9] ^= rotl32(z[5] + z[1], 7U);
        z[13] ^= rotl32(z[9] + z[5], 9U);
        z[1] ^= rotl32(z[13] + z[9], 13U);
        z[5] ^= rotl32(z[1] + z[13], 18U);

        z[14] ^= rotl32(z[10] + z[6], 7U);
        z[2] ^= rotl32(z[14] + z[10], 9U);
        z[6] ^= rotl32(z[2] + z[14], 13U);
        z[10] ^= rotl32(z[6] + z[2], 18U);

        z[3] ^= rotl32(z[15] + z[11], 7U);
        z[7] ^= rotl32(z[3] + z[15], 9U);
        z[11] ^= rotl32(z[7] + z[3], 13U);
        z[15] ^= rotl32(z[11] + z[7], 18U);

        z[1] ^= rotl32(z[0] + z[3], 7U);
        z[2] ^= rotl32(z[1] + z[0], 9U);
        z[3] ^= rotl32(z[2] + z[1], 13U);
        z[0] ^= rotl32(z[3] + z[2], 18U);

        z[6] ^= rotl32(z[5] + z[4], 7U);
        z[7] ^= rotl32(z[6] + z[5], 9U);
        z[4] ^= rotl32(z[7] + z[6], 13U);
        z[5] ^= rotl32(z[4] + z[7], 18U);

        z[11] ^= rotl32(z[10] + z[9], 7U);
        z[8] ^= rotl32(z[11] + z[10], 9U);
        z[9] ^= rotl32(z[8] + z[11], 13U);
        z[10] ^= rotl32(z[9] + z[8], 18U);

        z[12] ^= rotl32(z[15] + z[14], 7U);
        z[13] ^= rotl32(z[12] + z[15], 9U);
        z[14] ^= rotl32(z[13] + z[12], 13U);
        z[15] ^= rotl32(z[14] + z[13], 18U);
    }

    for (uint8_t i = 0U; i < 16U; i++) {
        z[i] += x[i];
        store32_le(&out[i * 4U], z[i]);
    }
}

static void hsalsa20(uint8_t out[32U],
                     const uint8_t key[32U],
                     const uint8_t nonce[16U])
{
    uint32_t x[16];
    uint32_t z[16];

    x[0]  = load32_le(&S20_SIGMA[0]);
    x[5]  = load32_le(&S20_SIGMA[4]);
    x[10] = load32_le(&S20_SIGMA[8]);
    x[15] = load32_le(&S20_SIGMA[12]);
    x[1]  = load32_le(&key[0]);
    x[2]  = load32_le(&key[4]);
    x[3]  = load32_le(&key[8]);
    x[4]  = load32_le(&key[12]);
    x[11] = load32_le(&key[16]);
    x[12] = load32_le(&key[20]);
    x[13] = load32_le(&key[24]);
    x[14] = load32_le(&key[28]);
    x[6]  = load32_le(&nonce[0]);
    x[7]  = load32_le(&nonce[4]);
    x[8]  = load32_le(&nonce[8]);
    x[9]  = load32_le(&nonce[12]);

    for (uint8_t i = 0U; i < 16U; i++) {
        z[i] = x[i];
    }

    for (uint8_t r = 0U; r < 10U; r++) {
        z[4] ^= rotl32(z[0] + z[12], 7U);
        z[8] ^= rotl32(z[4] + z[0], 9U);
        z[12] ^= rotl32(z[8] + z[4], 13U);
        z[0] ^= rotl32(z[12] + z[8], 18U);

        z[9] ^= rotl32(z[5] + z[1], 7U);
        z[13] ^= rotl32(z[9] + z[5], 9U);
        z[1] ^= rotl32(z[13] + z[9], 13U);
        z[5] ^= rotl32(z[1] + z[13], 18U);

        z[14] ^= rotl32(z[10] + z[6], 7U);
        z[2] ^= rotl32(z[14] + z[10], 9U);
        z[6] ^= rotl32(z[2] + z[14], 13U);
        z[10] ^= rotl32(z[6] + z[2], 18U);

        z[3] ^= rotl32(z[15] + z[11], 7U);
        z[7] ^= rotl32(z[3] + z[15], 9U);
        z[11] ^= rotl32(z[7] + z[3], 13U);
        z[15] ^= rotl32(z[11] + z[7], 18U);

        z[1] ^= rotl32(z[0] + z[3], 7U);
        z[2] ^= rotl32(z[1] + z[0], 9U);
        z[3] ^= rotl32(z[2] + z[1], 13U);
        z[0] ^= rotl32(z[3] + z[2], 18U);

        z[6] ^= rotl32(z[5] + z[4], 7U);
        z[7] ^= rotl32(z[6] + z[5], 9U);
        z[4] ^= rotl32(z[7] + z[6], 13U);
        z[5] ^= rotl32(z[4] + z[7], 18U);

        z[11] ^= rotl32(z[10] + z[9], 7U);
        z[8] ^= rotl32(z[11] + z[10], 9U);
        z[9] ^= rotl32(z[8] + z[11], 13U);
        z[10] ^= rotl32(z[9] + z[8], 18U);

        z[12] ^= rotl32(z[15] + z[14], 7U);
        z[13] ^= rotl32(z[12] + z[15], 9U);
        z[14] ^= rotl32(z[13] + z[12], 13U);
        z[15] ^= rotl32(z[14] + z[13], 18U);
    }

    store32_le(&out[0], z[0]);
    store32_le(&out[4], z[5]);
    store32_le(&out[8], z[10]);
    store32_le(&out[12], z[15]);
    store32_le(&out[16], z[6]);
    store32_le(&out[20], z[7]);
    store32_le(&out[24], z[8]);
    store32_le(&out[28], z[9]);
}

CeePewErr_t crypto_stream_init(StreamCipher_t *ctx,
                               const uint8_t key[32U],
                               const uint8_t nonce[24U])
{
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(key != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(nonce != NULL, CEEPEW_ERR_NULL_PTR);

    memset(ctx, 0, sizeof(*ctx));
    hsalsa20(ctx->subkey, key, nonce);
    memcpy(ctx->nonce_tail, nonce + 16U, 8U);
    ctx->block_counter = 0ULL;
    ctx->block_used = 64U;
    ctx->active = true;
    return CEEPEW_OK;
}

CeePewErr_t crypto_stream_process(StreamCipher_t *ctx,
                                 const uint8_t *in,
                                 uint16_t in_len,
                                 uint8_t *out,
                                 uint16_t *out_len)
{
    CEEPEW_ASSERT(ctx != NULL && ctx->active, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(*out_len >= in_len, CEEPEW_ERR_BOUNDS);

    for (uint16_t i = 0U; i < in_len; i++) {
        if (ctx->block_used >= 64U) {
            salsa20_core(ctx->block, ctx->subkey, ctx->nonce_tail, ctx->block_counter);
            ctx->block_counter++;
            ctx->block_used = 0U;
        }
        out[i] = (uint8_t)(in[i] ^ ctx->block[ctx->block_used]);
        ctx->block_used++;
    }

    *out_len = in_len;
    return CEEPEW_OK;
}

CeePewErr_t crypto_stream_finalise(StreamCipher_t *ctx)
{
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    ceepew_secure_zero(ctx, (uint32_t)sizeof(*ctx));
    return CEEPEW_OK;
}

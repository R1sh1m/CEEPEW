#include "crypto_hmac.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_security_utils.h"
#include <string.h>
#include <mbedtls/md.h>

CeePewErr_t crypto_hmac_sha256(const uint8_t *key, uint16_t key_len,
                               const uint8_t *msg, uint32_t msg_len,
                               uint8_t out[32])
{
    CEEPEW_ASSERT(key != NULL || key_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg != NULL || msg_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL, CEEPEW_ERR_NULL_PTR);

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) { return CEEPEW_ERR_CRYPTO; }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    int rc = mbedtls_md_setup(&ctx, info, 0);
    if (rc != 0) {
        mbedtls_md_free(&ctx);
        return CEEPEW_ERR_CRYPTO;
    }

    const size_t block_size = 64U;
    uint8_t key_block[64U];
    uint8_t inner_pad[64U];
    uint8_t outer_pad[64U];
    uint8_t inner_hash[32U];

    memset(key_block, 0U, sizeof(key_block));
    if (key_len > block_size) {
        rc = mbedtls_md_starts(&ctx);
        if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }
        rc = mbedtls_md_update(&ctx, key, key_len);
        if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }
        rc = mbedtls_md_finish(&ctx, key_block);
        if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }
    }
    else if (key_len > 0U) { memcpy(key_block, key, key_len); }

    for (uint8_t i = 0U; i < block_size; i++) {
        inner_pad[i] = (uint8_t)(key_block[i] ^ 0x36U);
        outer_pad[i] = (uint8_t)(key_block[i] ^ 0x5CU);
    }

    rc = mbedtls_md_starts(&ctx);
    if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }
    rc = mbedtls_md_update(&ctx, inner_pad, block_size);
    if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }
    rc = mbedtls_md_update(&ctx, msg, msg_len);
    if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }
    rc = mbedtls_md_finish(&ctx, inner_hash);
    if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }

    rc = mbedtls_md_starts(&ctx);
    if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }
    rc = mbedtls_md_update(&ctx, outer_pad, block_size);
    if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }
    rc = mbedtls_md_update(&ctx, inner_hash, sizeof(inner_hash));
    if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }
    rc = mbedtls_md_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    if (rc != 0) { return CEEPEW_ERR_CRYPTO; }

    volatile uint8_t *vk = (volatile uint8_t *)key_block;
    volatile uint8_t *vi = (volatile uint8_t *)inner_pad;
    volatile uint8_t *vo = (volatile uint8_t *)outer_pad;
    volatile uint8_t *vh = (volatile uint8_t *)inner_hash;
    for (uint32_t i = 0U; i < sizeof(key_block); i++) { vk[i] = 0U; }
    for (uint32_t i = 0U; i < sizeof(inner_pad); i++) { vi[i] = 0U; }
    for (uint32_t i = 0U; i < sizeof(outer_pad); i++) { vo[i] = 0U; }
    for (uint32_t i = 0U; i < sizeof(inner_hash); i++) { vh[i] = 0U; }
    __asm__ __volatile__("" ::: "memory");

    return CEEPEW_OK;
}
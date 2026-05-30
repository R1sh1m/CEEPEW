/* components/crypto/crypto_hkdf.c */

#include "crypto_hkdf.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <stdint.h>
#include <string.h>

#include <mbedtls/md.h>
/* Implement HKDF-Extract/Expand using a small HMAC-SHA256 implemented
 * on top of the one-shot mbedtls_sha256_ret() helper (avoids md context
 * types that may not be available in all IDF/mbedtls headers). */

static CeePewErr_t hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32U]) {
    CEEPEW_ASSERT(out != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(key != NULL || key_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg != NULL || msg_len == 0U, CEEPEW_ERR_NULL_PTR);

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

/* SECURITY: The HKDF salt must be SHA256(digital_sum_mix(code) || code) as
 * per Final Spec §3.3. The session code is the only secret the HKDF caller
 * holds — without it, ECDH shared secret alone is insufficient to derive the
 * session key.
 */
CeePewErr_t crypto_hkdf_build_info(const uint8_t *label, uint8_t label_len,
                                   const uint8_t id_a[6], const uint8_t id_b[6],
                                   const uint8_t commitment[32], uint32_t t_round,
                                   uint8_t *out_info, uint8_t *out_len)
{
    CEEPEW_ASSERT(label != NULL && label_len > 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(id_a != NULL && id_b != NULL && commitment != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out_info != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t off = 0U;
    memcpy(out_info + off, label, label_len); off += label_len;
    memcpy(out_info + off, id_a, 6U); off += 6U;
    memcpy(out_info + off, id_b, 6U); off += 6U;
    memcpy(out_info + off, commitment, 32U); off += 32U;

    /* Canonical t_round encoding: big-endian u32 */
    out_info[off++] = (uint8_t)((t_round >> 24) & 0xFFU);
    out_info[off++] = (uint8_t)((t_round >> 16) & 0xFFU);
    out_info[off++] = (uint8_t)((t_round >> 8) & 0xFFU);
    out_info[off++] = (uint8_t)(t_round & 0xFFU);

    *out_len = off;
    return CEEPEW_OK;
}

/* SECURITY: The HKDF salt must be SHA256(digital_sum_mix(code) || code) as
 * per Final Spec §3.3. The session code is the only secret the HKDF caller
 * holds — without it, ECDH shared secret alone is insufficient to derive the
 * session key.
 */
CeePewErr_t crypto_hkdf_derive(const uint8_t *ikm, uint8_t ikm_len, const uint8_t *salt, uint8_t salt_len, const uint8_t *info, uint8_t info_len, uint8_t *out, uint8_t out_len) {
    CEEPEW_ASSERT(ikm != NULL && ikm_len > 0U && ikm_len <= 32U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(out != NULL && out_len > 0U && out_len <= 64U, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(salt != NULL || salt_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(info != NULL || info_len == 0U, CEEPEW_ERR_NULL_PTR);

    uint8_t prk[32U];
    CeePewErr_t err = hmac_sha256(salt, (size_t)salt_len, ikm, (size_t)ikm_len, prk);
    if (err != CEEPEW_OK) {
        return err;
    }

    uint8_t t[32U];
    uint8_t generated = 0U;
    uint8_t previous_len = 0U;
    uint8_t nblocks = (uint8_t)((out_len + 31U) / 32U);
    uint8_t input_buf[160U];

    for (uint8_t block = 0U; block < nblocks; block++) {
        size_t pos = 0U;
        if (previous_len > 0U) {
            memcpy(input_buf + pos, t, previous_len);
            pos += previous_len;
        }
        if (info_len > 0U) {
            memcpy(input_buf + pos, info, info_len);
            pos += info_len;
        }
        input_buf[pos++] = (uint8_t)(block + 1U);

        err = hmac_sha256(prk, sizeof(prk), input_buf, pos, t);
        if (err != CEEPEW_OK) {
            volatile uint8_t *vp = (volatile uint8_t *)prk;
            for (uint32_t i = 0U; i < sizeof(prk); i++) { vp[i] = 0U; }
            __asm__ __volatile__("" ::: "memory");
            return err;
        }

        uint8_t to_copy = (uint8_t)((out_len - generated) < 32U ? (out_len - generated) : 32U);
        memcpy(out + generated, t, to_copy);
        generated = (uint8_t)(generated + to_copy);
        previous_len = 32U;
    }

    volatile uint8_t *vp = (volatile uint8_t *)prk;
    volatile uint8_t *vt = (volatile uint8_t *)t;
    for (uint32_t i = 0U; i < sizeof(prk); i++) { vp[i] = 0U; }
    for (uint32_t i = 0U; i < sizeof(t); i++) { vt[i] = 0U; }
    __asm__ __volatile__("" ::: "memory");

    return CEEPEW_OK;
}

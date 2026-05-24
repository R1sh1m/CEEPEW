/* components/crypto/crypto_pad.c */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <stdint.h>

#define CEEPEW_PAD_BLOCK_SIZE 64U

static void pad_write(uint8_t *out, uint16_t len, uint8_t pad_len){
    for (uint16_t i = 0U; i < pad_len; i++) { out[len + i] = pad_len;}
}

CeePewErr_t crypto_pad_apply(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len) {
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    uint16_t padded_len = (uint16_t)(((uint32_t)in_len / CEEPEW_PAD_BLOCK_SIZE + 1U) * CEEPEW_PAD_BLOCK_SIZE);
    if ((in_len % CEEPEW_PAD_BLOCK_SIZE) == 0U) {
        padded_len = (uint16_t)(in_len + CEEPEW_PAD_BLOCK_SIZE);
    }
    CEEPEW_ASSERT(padded_len <= *out_len, CEEPEW_ERR_BOUNDS);
    if (in_len > 0U) {
        for (uint16_t i = 0U; i < in_len; i++) { out[i] = in[i]; }
    }
    pad_write(out, in_len, (uint8_t)(padded_len - in_len));
    *out_len = padded_len;
    return CEEPEW_OK;
}

CeePewErr_t crypto_pad_remove(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len) {
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in_len > 0U && (in_len % CEEPEW_PAD_BLOCK_SIZE) == 0U, CEEPEW_ERR_PARAM);

    uint8_t pad_len = in[in_len - 1U];
    CEEPEW_ASSERT(pad_len > 0U && pad_len <= CEEPEW_PAD_BLOCK_SIZE, CEEPEW_ERR_CRYPTO);
    CEEPEW_ASSERT((uint16_t)pad_len <= in_len, CEEPEW_ERR_CRYPTO);
    CEEPEW_ASSERT((uint16_t)(in_len - pad_len) <= *out_len, CEEPEW_ERR_BOUNDS);

    uint8_t bad = 0U;
    for (uint8_t i = 0U; i < CEEPEW_PAD_BLOCK_SIZE; i++) {
        uint8_t mask = (uint8_t)((i < pad_len) ? 0xFFU : 0x00U);
        bad |= (uint8_t)((in[in_len - 1U - i] ^ pad_len) & mask);
    }
    CEEPEW_ASSERT(bad == 0U, CEEPEW_ERR_CRYPTO);

    uint16_t plain_len = (uint16_t)(in_len - pad_len);
    for (uint16_t i = 0U; i < plain_len; i++) { out[i] = in[i]; }
    *out_len = plain_len;
    return CEEPEW_OK;
}

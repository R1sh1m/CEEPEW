/* components/crypto/crypto_rng.c */

#include "crypto_rng.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "esp_system.h"
#include "esp_random.h"
#include <stddef.h>
#include <stdint.h>

#define CEEPEW_RNG_CHUNK_BYTES 32U

static CeePewErr_t rng_fill_esp(uint8_t *buf, uint32_t len) {
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_BOUNDS);
    uint32_t offset = 0U;
    /* Fill using esp_random() (returns 32-bit random). Copy in 4-byte chunks. */
    while (offset < len) {
        uint32_t rnd = esp_random();
        uint32_t remain = len - offset;
        uint32_t to_copy = (remain < 4U) ? remain : 4U;
        /* copy little-endian bytes of rnd into buffer */
        for (uint32_t i = 0U; i < to_copy; i++) {
            buf[offset + i] = (uint8_t)((rnd >> (8U * i)) & 0xFFU);
        }
        offset += to_copy;
    }

    uint8_t non_zero_acc = 0U;
    /* loop bound: len validated <= CEEPEW_REGION_POOL_BYTES */
    for (uint32_t i = 0U; i < len; i++) { non_zero_acc |= buf[i]; }
    CEEPEW_ASSERT(non_zero_acc != 0U, CEEPEW_ERR_CRYPTO);
    return CEEPEW_OK;
}

CeePewErr_t crypto_rng_fill(uint8_t *buf, uint32_t len) {
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_BOUNDS);
    return rng_fill_esp(buf, len);
}

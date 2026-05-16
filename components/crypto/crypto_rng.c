/* components/crypto/crypto_rng.c */

#include "crypto_rng.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include "esp_system.h"
#include <stddef.h>
#include <stdint.h>

#define CEEPEW_RNG_CHUNK_BYTES 32U

static CeePewErr_t rng_fill_esp(uint8_t *buf, uint32_t len)
{
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_BOUNDS);

    uint32_t offset = 0U;
    /* loop bound: len validated <= CEEPEW_REGION_POOL_BYTES */
    for (; offset < len; offset += CEEPEW_RNG_CHUNK_BYTES)
    {
        uint32_t remain = len - offset;
        uint32_t chunk = (remain < CEEPEW_RNG_CHUNK_BYTES) ? remain : CEEPEW_RNG_CHUNK_BYTES;
        esp_fill_random(&buf[offset], (size_t)chunk);
    }

    uint8_t non_zero_acc = 0U;
    /* loop bound: len validated <= CEEPEW_REGION_POOL_BYTES */
    for (uint32_t i = 0U; i < len; i++)
    {
        non_zero_acc |= buf[i];
    }
    CEEPEW_ASSERT(non_zero_acc != 0U, CEEPEW_ERR_CRYPTO);

    return CEEPEW_OK;
}

CeePewErr_t crypto_rng_fill(uint8_t *buf, uint32_t len)
{
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_BOUNDS);

    return rng_fill_esp(buf, len);
}

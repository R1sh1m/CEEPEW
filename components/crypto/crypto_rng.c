/* components/crypto/crypto_rng.c */

#include "crypto_rng.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include <stdint.h>
#include <string.h>

#include "esp_system.h"  /* esp_fill_random */

#define CEEPEW_RNG_SAMPLE_BYTES 16U
#define CEEPEW_RNG_MAX_BYTES 512U

CeePewErr_t crypto_rng_fill(uint8_t *buf, uint32_t len)
{
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_RNG_MAX_BYTES, CEEPEW_ERR_BOUNDS);

    esp_fill_random(buf, (size_t)len);

    /* Verify not all-zero on first sample bytes */
    uint8_t acc = 0U;
    uint32_t sample = (len < CEEPEW_RNG_SAMPLE_BYTES) ? len : CEEPEW_RNG_SAMPLE_BYTES;
    /* Compile-time bounded loop up to CEEPEW_RNG_SAMPLE_BYTES to satisfy loop-bound rule */
    for (uint32_t i = 0U; i < CEEPEW_RNG_SAMPLE_BYTES; i++) {
        if (i >= sample) { break; }
        acc |= buf[i];
    }

    if (acc == 0U) {
        return CEEPEW_ERR_CRYPTO;
    }

    return CEEPEW_OK;
}

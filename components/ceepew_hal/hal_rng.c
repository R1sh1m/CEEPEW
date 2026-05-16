/* components/ceepew_hal/hal_rng.c */

#include "hal_adc.h"
#include "../../components/crypto/crypto_rng.h"
#include "../../main/ceepew_assert.h"
#include <stdint.h>

CeePewErr_t hal_rng_fill(uint8_t *buf, uint32_t len)
{
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= 512U, CEEPEW_ERR_BOUNDS);

    /* Delegate to crypto RNG which uses esp_fill_random under the hood */
    return crypto_rng_fill(buf, len);
}

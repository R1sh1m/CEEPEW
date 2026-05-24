/* components/ceepew_hal/hal_rng.c */

#include "hal_rng.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"

#include "esp_random.h"

#include <stdint.h>
#include <stdbool.h>

static bool s_initialised = false;

CeePewErr_t hal_rng_init(void)
{
    CEEPEW_ASSERT(!s_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(CEEPEW_REGION_POOL_BYTES >= 64U, CEEPEW_ERR_PARAM);

    /* Warm up the hardware RNG so the first post-WiFi reads are not stale. */
    for (uint8_t i = 0U; i < 8U; i++) {
        (void)esp_random();
    }

    s_initialised = true;
    return CEEPEW_OK;
}

CeePewErr_t hal_rng_fill(uint8_t *buf, uint32_t len)
{
    CEEPEW_ASSERT(s_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= 512U, CEEPEW_ERR_BOUNDS);

    uint32_t offset = 0U;
    while (offset < len) {
        uint32_t rnd = esp_random();
        for (uint8_t i = 0U; i < 4U && offset < len; i++) {
            buf[offset] = (uint8_t)(rnd & 0xFFU);
            rnd >>= 8U;
            offset++;
        }
    }
    return CEEPEW_OK;
}

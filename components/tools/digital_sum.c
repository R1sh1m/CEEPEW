/* components/tools/digital_sum.c */

#include "digital_sum.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include <stdint.h>

/*
 * SHA256(digital_sum_mix(code) || code) forms the HKDF salt for every session key.
 * This algorithm is protocol-bound: removing it requires a version bump.
 */

uint8_t digital_sum_reduce(const uint8_t *data, uint16_t len)
{
    CEEPEW_ASSERT(data != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);

    uint32_t total = 0U;
    /* loop bound: len <= CEEPEW_MAX_MSG_BYTES */
    for (uint16_t i = 0U; i < len; i++)
    {
        total += (uint32_t)data[i];
    }

    /* Maximum iterations: <10 for uint32_t digit-root convergence */
    for (uint8_t iter = 0U; iter < 10U && total >= 10U; iter++)
    {
        uint32_t digit_sum = 0U;
        uint32_t tmp = total;
        /* loop bound: uint32 has <=10 decimal digits */
        for (uint8_t d = 0U; d < 10U && tmp > 0U; d++)
        {
            digit_sum += (tmp % 10U);
            tmp /= 10U;
        }
        total = digit_sum;
    }

    if ((total % 9U) == 0U && total != 0U)
    {
        return 9U;
    }
    return (uint8_t)(total % 9U);
}

void digital_sum_mix(const uint8_t *in, uint16_t len, uint8_t out[32])
{
    CEEPEW_ASSERT_VOID(in != NULL);
    CEEPEW_ASSERT_VOID(len > 0U && len <= CEEPEW_MAX_MSG_BYTES);
    CEEPEW_ASSERT_VOID(out != NULL);

    /* loop bound: 32 */
    for (uint8_t i = 0U; i < 32U; i++)
    {
        out[i] = 0U;
    }

    /* Slide a 9-byte window with unit step across all starts [0, len-1]. */
    /* loop bound: len <= CEEPEW_MAX_MSG_BYTES */
    for (uint16_t w = 0U; w < len; w++)
    {
        uint16_t window_end = (uint16_t)(((uint32_t)w + 9U < len) ? ((uint32_t)w + 9U) : len);
        uint16_t window_len = (uint16_t)(window_end - w);
        uint8_t r = digital_sum_reduce(in + w, window_len);
        uint8_t idx = (uint8_t)(w % 32U);
        out[idx] ^= r;
    }

    /* Final digital-root normalization pass. */
    /* loop bound: 32 */
    for (uint8_t i = 0U; i < 32U; i++)
    {
        out[i] = digital_sum_reduce(&out[i], 1U);
    }
}

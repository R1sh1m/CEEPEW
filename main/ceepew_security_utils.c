/* main/ceepew_security_utils.c */

#include "ceepew_security_utils.h"

#include <stddef.h>

void ceepew_secure_zero(volatile void *ptr, uint32_t len)
{
    if (ptr == NULL || len == 0U) {
        return;
    }

    volatile uint8_t *p = (volatile uint8_t *)ptr;
    for (uint32_t i = 0U; i < len; i++) {
        p[i] = 0U;
    }
    __asm__ __volatile__("" ::: "memory");
}

uint8_t ceepew_ct_equal(const uint8_t *a, const uint8_t *b, uint32_t len)
{
    if (a == NULL || b == NULL) {
        return 0U;
    }

    uint8_t diff = 0U;
    for (uint32_t i = 0U; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return (uint8_t)(diff == 0U);
}

/* Constant-time lexicographic comparison: a < b?
 * Constant across all byte sequences regardless of equality.
 * Used for deterministic but timing-safe role determination in pairing.
 */
uint8_t ceepew_ct_less(const uint8_t *a, const uint8_t *b, uint32_t len)
{
    if (a == NULL || b == NULL) {
        return 0U;
    }

    uint8_t less_than = 0U;
    uint8_t seen_diff = 0U;

    /* Scan all bytes, accumulating comparison info in constant time */
    for (uint32_t i = 0U; i < len; i++) {
        uint8_t diff = (uint8_t)(a[i] ^ b[i]);
        /* If we haven't seen a difference yet, check if a[i] < b[i] */
        uint8_t a_less = (uint8_t)((a[i] < b[i]) ? 1U : 0U);
        /* Update result only if this is the first difference and a < b */
        less_than |= ((uint8_t)(1U - seen_diff) & a_less);
        /* Mark that we've seen a difference */
        seen_diff |= (uint8_t)((diff != 0U) ? 1U : 0U);
    }

    return less_than;
}

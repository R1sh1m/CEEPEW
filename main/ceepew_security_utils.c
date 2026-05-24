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

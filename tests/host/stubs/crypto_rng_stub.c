/* tests/host/stubs/crypto_rng_stub.c — stubs for crypto_rng_* functions so
 * crypto_eddsa.c (which calls crypto_rng_fill in its keypair path) can be
 * linked on the host.  The test only calls seeded_keypair, so these stubs
 * are never actually reached in practice. */
#include "crypto_rng.h"
#include <string.h>

CeePewErr_t crypto_rng_fill(uint8_t *buf, uint32_t len)
{
    if (buf == NULL && len > 0U) {
        return CEEPEW_ERR_NULL_PTR;
    }
    static uint32_t s_state = 0x9E3779B9U;
    for (uint32_t i = 0U; i < len; i++) {
        s_state = s_state * 1664525U + 1013904223U;
        buf[i] = (uint8_t)(s_state >> 24U);
    }
    return CEEPEW_OK;
}

CeePewErr_t crypto_rng_health_check(void)
{
    return CEEPEW_OK;
}

void crypto_rng_set_failure_callback(crypto_rng_failure_cb_t cb) { (void)cb; }

CeePewErr_t crypto_rng_continuous_test(const uint8_t *sample, uint32_t len)
{
    (void)sample; (void)len;
    return CEEPEW_OK;
}

uint32_t crypto_rng_get_failure_count(void) { return 0U; }

void crypto_rng_reset_health_state(void) {}

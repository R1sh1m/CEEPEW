/* components/crypto/crypto_rng.h */

#ifndef CRYPTO_RNG_H
#define CRYPTO_RNG_H

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <stdint.h>

CeePewErr_t crypto_rng_fill(uint8_t *buf, uint32_t len);
CeePewErr_t crypto_rng_health_check(void);

/* Continuous health test (NIST SP 800-90B inspired) */
typedef void (*crypto_rng_failure_cb_t)(void);

void crypto_rng_set_failure_callback(crypto_rng_failure_cb_t cb);
CeePewErr_t crypto_rng_continuous_test(const uint8_t *sample, uint32_t len);
uint32_t crypto_rng_get_failure_count(void);
void crypto_rng_reset_health_state(void);

#endif /* CRYPTO_RNG_H */

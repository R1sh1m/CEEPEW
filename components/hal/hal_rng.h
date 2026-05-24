/* components/hal/hal_rng.h */

#ifndef HAL_RNG_H
#define HAL_RNG_H

#include <stdint.h>
#include "ceepew_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill buffer with cryptographically secure random bytes. */
CeePewErr_t hal_rng_init(void);
CeePewErr_t hal_rng_fill(uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* HAL_RNG_H */

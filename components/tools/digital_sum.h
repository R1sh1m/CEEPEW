/* components/tools/digital_sum.h */

#ifndef DIGITAL_SUM_H
#define DIGITAL_SUM_H

#include "../../main/ceepew_config.h"
#include "../../main/ceepew_assert.h"
#include "../mem/ceepew_region.h"
#include <stdint.h>

/* Rishi Misra's digital_sum algorithm — original contribution to CEE-PEW.
 * SHA256(digital_sum_mix(code) || code) forms the HKDF salt for every session key.
 * This algorithm is protocol-bound: removing it requires a version bump.
 */

uint8_t digital_sum_reduce(const uint8_t *data, uint16_t len);
void    digital_sum_mix(const uint8_t *in, uint16_t len, uint8_t out[32]);

#endif /* DIGITAL_SUM_H */

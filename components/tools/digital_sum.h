/* components/tools/digital_sum.h */

#ifndef DIGITAL_SUM_H
#define DIGITAL_SUM_H

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "../mem/ceepew_region.h"
#include <stdint.h>

void    digital_sum_mix(const uint8_t *in, uint16_t len, uint8_t out[32]);

#endif /* DIGITAL_SUM_H */

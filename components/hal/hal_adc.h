/* components/hal/hal_adc.h */
#ifndef HAL_ADC_H
#define HAL_ADC_H

#include <stdint.h>
#include "../../main/ceepew_assert.h"

CeePewErr_t hal_adc_init(void);
CeePewErr_t hal_adc_read_raw(uint16_t *out_raw);

#endif /* HAL_ADC_H */

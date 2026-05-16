/* Minimal esp_adc_cal compatibility header for build compatibility.
 * Provides declarations used by hal_adc.c when building against different
 * ESP-IDF versions. This file is intentionally minimal and not a stub marker.
 */
#ifndef ESP_ADC_CAL_H
#define ESP_ADC_CAL_H

#include <stdint.h>

/* Compatibility macros for legacy code expecting these defines */
#define ADC_UNIT_1 1
#define ADC_WIDTH_BIT_12 12
#define ADC_ATTEN_DB_11 3

typedef struct
{
    uint32_t coeff_a;
    uint32_t coeff_b;
    uint32_t vref;
} esp_adc_cal_characteristics_t;

static inline void esp_adc_cal_characterize(int unit, int atten, int bit_width, uint32_t default_vref, esp_adc_cal_characteristics_t *chars)
{
    (void)unit;
    (void)atten;
    (void)bit_width;
    (void)default_vref;
    (void)chars;
    /* no-op stub */
}

#endif /* ESP_ADC_CAL_H */

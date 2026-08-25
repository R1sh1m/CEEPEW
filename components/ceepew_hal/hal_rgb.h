#ifndef HAL_RGB_H
#define HAL_RGB_H

#include "rgb_pattern.h"
#include "hal_ui_types.h"

CeePewErr_t rgb_init(void);
CeePewErr_t rgb_set_pattern(RgbPattern_t pattern);
CeePewErr_t rgb_task(void);

/* Set LED to smooth PWM pulsing mode with specified intensities (0-255) */
CeePewErr_t rgb_set_pwm_mode(uint8_t r_intensity, uint8_t g_intensity, uint8_t b_intensity);

/* High-level API: smoothly pulse LED at 1 Hz with specified color */
CeePewErr_t rgb_pulse(uint8_t r_intensity, uint8_t g_intensity, uint8_t b_intensity);

#endif /* HAL_RGB_H */

#ifndef HAL_RGB_H
#define HAL_RGB_H

#include <stdint.h>
#include "ceepew_assert.h"

typedef enum {
    RGB_OFF = 0,
    RGB_RED,
    RGB_GREEN,
    RGB_BLUE,
    RGB_YELLOW,
    RGB_CYAN,
    RGB_MAGENTA,
    RGB_WHITE,
    RGB_RED_BLINK,
    RGB_GREEN_BLINK,
    RGB_BLUE_BLINK,
    RGB_WHITE_PULSE,        /* Smooth PWM breathing white */
    RGB_BLUE_PULSE,         /* Smooth PWM breathing blue  */
    RGB_GREEN_PULSE,        /* Smooth PWM breathing green */
    RGB_AMBER_PULSE,        /* Smooth PWM breathing amber */
    RGB_CYAN_PULSE,         /* Smooth PWM breathing cyan  */
    RGB_RAINBOW_CYCLE,
    RGB_HEARTBEAT,
    RGB_PATTERN_COUNT
} RgbPattern_t;

CeePewErr_t rgb_init(void);
CeePewErr_t rgb_set_pattern(RgbPattern_t pattern);
CeePewErr_t rgb_task(void);

/* Set LED to smooth PWM pulsing mode with specified intensities (0-255) */
CeePewErr_t rgb_set_pwm_mode(uint8_t r_intensity, uint8_t g_intensity, uint8_t b_intensity);

/* High-level API: smoothly pulse LED at 1 Hz with specified color */
CeePewErr_t rgb_pulse(uint8_t r_intensity, uint8_t g_intensity, uint8_t b_intensity);

#endif /* HAL_RGB_H */

/* components/ceepew_common/rgb_pattern.h
 *
 * Shared RGB LED pattern types.
 * Single source of truth for RgbPattern_t enum used by both
 * ceepew_hal (hal_rgb) and transport (transport_ble).
 */

#ifndef CEEPEW_RGB_PATTERN_H
#define CEEPEW_RGB_PATTERN_H

#include <stdint.h>
#include "hal_ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RGB LED patterns (per Technical Specification §4 — RGB LED Patterns).
 * Used by hal_rgb.c for actual LED driving and transport_ble.c for
 * phase-to-pattern mapping. */
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
    RGB_RED_PULSE,          /* Smooth PWM breathing red for error states */
    RGB_WHITE_PULSE,        /* Smooth PWM breathing white */
    RGB_BLUE_PULSE,         /* Smooth PWM breathing blue  */
    RGB_GREEN_PULSE,        /* Smooth PWM breathing green */
    RGB_AMBER_PULSE,        /* Smooth PWM breathing amber */
    RGB_CYAN_PULSE,         /* Smooth PWM breathing cyan  */
    RGB_YELLOW_RED_BLINK,   /* Alternating yellow/red blink — supervisor recovery indicator */
    RGB_CYAN_BLINK,         /* Steady cyan blink — GATT identity exchange in progress */
    RGB_RAINBOW_CYCLE,
    RGB_HEARTBEAT,
    RGB_PATTERN_COUNT
} RgbPattern_t;

#ifdef __cplusplus
}
#endif

#endif /* CEEPEW_RGB_PATTERN_H */
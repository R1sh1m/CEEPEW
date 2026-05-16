#ifndef HAL_RGB_H
#define HAL_RGB_H

#include <stdint.h>
#include "../../main/ceepew_assert.h"

typedef enum
{
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
    RGB_AMBER_PULSE,
    RGB_CYAN_PULSE,
    RGB_RAINBOW_CYCLE,
    RGB_HEARTBEAT,
    RGB_PATTERN_COUNT
} RgbPattern_t;

CeePewErr_t rgb_init(void);
CeePewErr_t rgb_set_pattern(RgbPattern_t pattern);
CeePewErr_t rgb_task(void);

#endif /* HAL_RGB_H */

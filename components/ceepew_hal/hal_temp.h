/* components/ceepew_hal/hal_temp.h
 *
 * Die-temperature sensor driver for ESP32-WROOM-32.
 *
 * Hardware: the on-die temperature sensor in the ESP32 is exposed by a
 * ROM-resident routine (no peripheral driver is required and the
 * legacy `driver/temp_sensor.h` was removed in ESP-IDF v5+). The ROM
 * symbol is misspelled in the silicon as `temprature_sens_read` and
 * returns the die temperature in degrees Fahrenheit as a `uint8_t`.
 *
 * Typical accuracy: ±10 °C, biased upward by radio (WiFi/BT)
 * self-heating. The value is therefore a *die* temperature, not a
 * board ambient — call out that fact to anyone consuming the reading.
 *
 * The conversion is: Celsius = (Fahrenheit - 32) / 1.8.
 */
#ifndef CEEPEW_HAL_TEMP_H
#define CEEPEW_HAL_TEMP_H

#include "ceepew_assert.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

CeePewErr_t hal_temp_init(void);
bool        hal_temp_is_ready(void);
bool        hal_temp_read_celsius(float *out_celsius);

#ifdef __cplusplus
}
#endif

#endif /* CEEPEW_HAL_TEMP_H */

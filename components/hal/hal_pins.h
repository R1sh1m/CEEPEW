/* components/hal/hal_pins.h
 *
 * CEE-PEW Hardware Pin Configuration — SINGLE SOURCE OF TRUTH
 * All GPIO assignments reflect actual PCB wiring.
 * NO other file in the project may contain raw GPIO numbers.
 *
 * Wiring is corrected to the spec:
 *   OLED SDA → GPIO26 (was 21, changed to fix I2C conflicts)
 *   OLED SCL → GPIO27 (was 22, changed to fix I2C conflicts)
 *   POT      → GPIO33
 *   BUTTON   → GPIO19
 *   DIAG SW  → GPIO5
 *   RGB R    → GPIO15
 *   RGB G    → GPIO18
 *   RGB B    → GPIO23
 */

#ifndef HAL_PINS_H
#define HAL_PINS_H

#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_adc/adc_oneshot.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"

/* ── OLED Display (SSD1306, I2C) ─────────────────────────────────── */
#define CEEPEW_PIN_I2C_SDA          GPIO_NUM_26
#define CEEPEW_PIN_I2C_SCL          GPIO_NUM_27
#define CEEPEW_I2C_PORT             ((i2c_port_t)0)
#define CEEPEW_I2C_FREQ_HZ          400000U
#define CEEPEW_OLED_I2C_ADDR        0x3CU
#define CEEPEW_OLED_I2C_ADDR_FB     0x3DU   /* fallback address */

/* ── Rotary Potentiometer ─────────────────────────────────────────── */
#define CEEPEW_PIN_POT              GPIO_NUM_33
#define CEEPEW_ADC_UNIT             ADC_UNIT_1
#define CEEPEW_ADC_CHANNEL_POT      ADC_CHANNEL_5   /* ADC1_CH5 for GPIO33 */
#define CEEPEW_ADC_ATTEN            ADC_ATTEN_DB_12
#define CEEPEW_ADC_WIDTH            ADC_BITWIDTH_12

/* ── Click Button (SPST-NO, INPUT_PULLUP, active LOW) ────────────── */
/*    GPIO19 is input-capable and matches the locked v1 wiring.      */
#define CEEPEW_PIN_BUTTON           GPIO_NUM_19
#define CEEPEW_BUTTON_ACTIVE_LEVEL  0
#define CEEPEW_BUTTON_DEBOUNCE_MS   25U

/* ── Push-Lock DIAG Switch (INPUT_PULLUP, active LOW while held) ─── */
#define CEEPEW_PIN_DIAG_SWITCH      GPIO_NUM_5
#define CEEPEW_DIAG_SWITCH_ACTIVE   0

/* ── RGB LED (common-cathode, 3×220Ω to 3.3V) ───────────────────── */
#define CEEPEW_PIN_RGB_RED          GPIO_NUM_15
#define CEEPEW_PIN_RGB_GREEN        GPIO_NUM_18
#define CEEPEW_PIN_RGB_BLUE         GPIO_NUM_23

/* ── Radio (internal, no GPIO required) ──────────────────────────── */
#define CEEPEW_BLE_ADV_INTERVAL_MS  100U

/* ── Compile-time pin conflict checks ───────────────────────────── */
#define CEEPEW_PINS_ASSERT_UNIQUE()                                                      \
    do {                                                                                 \
        _Static_assert(CEEPEW_PIN_I2C_SDA != CEEPEW_PIN_I2C_SCL,                        \
                       "I2C SDA and SCL must differ");                                   \
        _Static_assert(CEEPEW_PIN_I2C_SDA != CEEPEW_PIN_POT,                            \
                       "I2C SDA conflicts with potentiometer");                          \
        _Static_assert(CEEPEW_PIN_I2C_SCL != CEEPEW_PIN_POT,                            \
                       "I2C SCL conflicts with potentiometer");                          \
        _Static_assert(CEEPEW_PIN_POT != CEEPEW_PIN_BUTTON,                             \
                       "Potentiometer conflicts with button");                           \
        _Static_assert(CEEPEW_PIN_BUTTON != CEEPEW_PIN_DIAG_SWITCH,                     \
                       "Button conflicts with DIAG switch");                             \
        _Static_assert(CEEPEW_PIN_BUTTON != CEEPEW_PIN_RGB_GREEN,                       \
                       "Button conflicts with RGB green");                               \
        _Static_assert(CEEPEW_PIN_BUTTON != CEEPEW_PIN_RGB_BLUE,                        \
                       "Button conflicts with RGB blue");                                \
        _Static_assert(CEEPEW_PIN_DIAG_SWITCH != CEEPEW_PIN_RGB_RED,                    \
                       "DIAG switch conflicts with RGB red");                            \
        _Static_assert(CEEPEW_PIN_DIAG_SWITCH != CEEPEW_PIN_RGB_GREEN,                  \
                       "DIAG switch conflicts with RGB green");                          \
        _Static_assert(CEEPEW_PIN_DIAG_SWITCH != CEEPEW_PIN_RGB_BLUE,                   \
                       "DIAG switch conflicts with RGB blue");                           \
        _Static_assert(CEEPEW_PIN_RGB_RED   != CEEPEW_PIN_RGB_GREEN, "RGB R=G");         \
        _Static_assert(CEEPEW_PIN_RGB_RED   != CEEPEW_PIN_RGB_BLUE,  "RGB R=B");         \
        _Static_assert(CEEPEW_PIN_RGB_GREEN != CEEPEW_PIN_RGB_BLUE,  "RGB G=B");         \
        _Static_assert(CEEPEW_ADC_UNIT == ADC_UNIT_1,                                    \
                       "Potentiometer must use ADC1");                                    \
        _Static_assert(CEEPEW_PIN_RGB_RED != GPIO_NUM_2,                                 \
                       "RGB Red must not be GPIO2");                                     \
    } while (0)

CeePewErr_t hal_pins_validate(void);

#endif /* HAL_PINS_H */

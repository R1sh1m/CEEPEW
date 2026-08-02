/* components/hal/hal_pins.h
 *
 * CEE-PEW Hardware Pin Configuration — SINGLE SOURCE OF TRUTH
 * All GPIO assignments reflect actual PCB wiring.
 * NO other file in the project may contain raw GPIO numbers.
 *
 * Wiring is corrected to the active board mapping:
 *   OLED SDA → GPIO26
 *   OLED SCL → GPIO27
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
#include "esp_adc/adc_oneshot.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"

/* ── OLED Display (SSD1306, I2C) ─────────────────────────────────── */
#ifdef CONFIG_CEEPEW_PIN_I2C_SDA
#define CEEPEW_PIN_I2C_SDA          ((gpio_num_t)CONFIG_CEEPEW_PIN_I2C_SDA)
#else
#define CEEPEW_PIN_I2C_SDA          GPIO_NUM_26
#endif

#ifdef CONFIG_CEEPEW_PIN_I2C_SCL
#define CEEPEW_PIN_I2C_SCL          ((gpio_num_t)CONFIG_CEEPEW_PIN_I2C_SCL)
#else
#define CEEPEW_PIN_I2C_SCL          GPIO_NUM_27
#endif

#ifdef CONFIG_CEEPEW_PIN_I2C_SDA_FALLBACK
#define CEEPEW_PIN_I2C_SDA_FALLBACK ((gpio_num_t)CONFIG_CEEPEW_PIN_I2C_SDA_FALLBACK)
#else
#define CEEPEW_PIN_I2C_SDA_FALLBACK GPIO_NUM_21
#endif

#ifdef CONFIG_CEEPEW_PIN_I2C_SCL_FALLBACK
#define CEEPEW_PIN_I2C_SCL_FALLBACK ((gpio_num_t)CONFIG_CEEPEW_PIN_I2C_SCL_FALLBACK)
#else
#define CEEPEW_PIN_I2C_SCL_FALLBACK GPIO_NUM_22
#endif

#define CEEPEW_I2C_PORT             ((i2c_port_t)1)
#define CEEPEW_I2C_FREQ_HZ          800000U
#define CEEPEW_I2C_FREQ_FALLBACK_HZ 400000U
#define CEEPEW_OLED_I2C_PROBE_TIMEOUT_MS 20U
#define CEEPEW_OLED_I2C_SCAN_ADDR_MIN 0x03U
#define CEEPEW_OLED_I2C_SCAN_ADDR_MAX 0x77U
#define CEEPEW_OLED_I2C_ADDR        0x3CU
#define CEEPEW_OLED_I2C_ADDR_FB     0x3DU   /* fallback address */

/* ── Rotary Potentiometer ─────────────────────────────────────────── */
/* NOTE: CEEPEW_PIN_POT_NUM is a raw integer (no cast) for use in #if
 * preprocessor directives. CEEPEW_PIN_POT includes the gpio_num_t cast
 * for use in C code. The _NUM variant must be used in #if chains because
 * the preprocessor does not understand type casts. */
#ifdef CONFIG_CEEPEW_PIN_POT
#define CEEPEW_PIN_POT_NUM          CONFIG_CEEPEW_PIN_POT
#define CEEPEW_PIN_POT              ((gpio_num_t)CEEPEW_PIN_POT_NUM)
#else
#define CEEPEW_PIN_POT_NUM          33
#define CEEPEW_PIN_POT              ((gpio_num_t)CEEPEW_PIN_POT_NUM)
#endif

#define CEEPEW_ADC_UNIT             ADC_UNIT_1

#if (CEEPEW_PIN_POT_NUM == 36)
#define CEEPEW_ADC_CHANNEL_POT      ADC_CHANNEL_0
#elif (CEEPEW_PIN_POT_NUM == 37)
#define CEEPEW_ADC_CHANNEL_POT      ADC_CHANNEL_1
#elif (CEEPEW_PIN_POT_NUM == 38)
#define CEEPEW_ADC_CHANNEL_POT      ADC_CHANNEL_2
#elif (CEEPEW_PIN_POT_NUM == 39)
#define CEEPEW_ADC_CHANNEL_POT      ADC_CHANNEL_3
#elif (CEEPEW_PIN_POT_NUM == 32)
#define CEEPEW_ADC_CHANNEL_POT      ADC_CHANNEL_4
#elif (CEEPEW_PIN_POT_NUM == 33)
#define CEEPEW_ADC_CHANNEL_POT      ADC_CHANNEL_5
#elif (CEEPEW_PIN_POT_NUM == 34)
#define CEEPEW_ADC_CHANNEL_POT      ADC_CHANNEL_6
#elif (CEEPEW_PIN_POT_NUM == 35)
#define CEEPEW_ADC_CHANNEL_POT      ADC_CHANNEL_7
#else
#define CEEPEW_ADC_CHANNEL_POT      ADC_CHANNEL_5
#endif

#define CEEPEW_ADC_ATTEN            ADC_ATTEN_DB_12
#define CEEPEW_ADC_WIDTH            ADC_BITWIDTH_12

/* ── Click Button (SPST-NO, INPUT_PULLUP, active LOW) ────────────── */
#ifdef CONFIG_CEEPEW_PIN_BUTTON
#define CEEPEW_PIN_BUTTON           ((gpio_num_t)CONFIG_CEEPEW_PIN_BUTTON)
#else
#define CEEPEW_PIN_BUTTON           GPIO_NUM_19
#endif
#define CEEPEW_BUTTON_ACTIVE_LEVEL  0
#define CEEPEW_BUTTON_DEBOUNCE_MS   25U

/* ── Push-Lock DIAG Switch (INPUT_PULLUP, active LOW while held) ─── */
#ifdef CONFIG_CEEPEW_PIN_DIAG_SWITCH
#define CEEPEW_PIN_DIAG_SWITCH      ((gpio_num_t)CONFIG_CEEPEW_PIN_DIAG_SWITCH)
#else
#define CEEPEW_PIN_DIAG_SWITCH      GPIO_NUM_5
#endif
#define CEEPEW_DIAG_SWITCH_ACTIVE   0

/* ── RGB LED (common-cathode, 3×220Ω to 3.3V) ───────────────────── */
#ifdef CONFIG_CEEPEW_PIN_RGB_RED
#define CEEPEW_PIN_RGB_RED          ((gpio_num_t)CONFIG_CEEPEW_PIN_RGB_RED)
#else
#define CEEPEW_PIN_RGB_RED          GPIO_NUM_15
#endif

#ifdef CONFIG_CEEPEW_PIN_RGB_GREEN
#define CEEPEW_PIN_RGB_GREEN        ((gpio_num_t)CONFIG_CEEPEW_PIN_RGB_GREEN)
#else
#define CEEPEW_PIN_RGB_GREEN        GPIO_NUM_18
#endif

#ifdef CONFIG_CEEPEW_PIN_RGB_BLUE
#define CEEPEW_PIN_RGB_BLUE         ((gpio_num_t)CONFIG_CEEPEW_PIN_RGB_BLUE)
#else
#define CEEPEW_PIN_RGB_BLUE         GPIO_NUM_23
#endif

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

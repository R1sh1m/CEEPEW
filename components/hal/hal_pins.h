/* components/hal/hal_pins.h */
#ifndef HAL_PINS_H
#define HAL_PINS_H

#include <driver/gpio.h>
#include <driver/adc.h>
#include <driver/i2c.h>
#include <esp_wifi.h>

/* ── OLED Display (SSD1306, I2C) ─────────────────────────────────── */
#define CEEPEW_PIN_I2C_SDA          GPIO_NUM_21
#define CEEPEW_PIN_I2C_SCL          GPIO_NUM_22
#define CEEPEW_I2C_PORT             I2C_NUM_0
#define CEEPEW_I2C_FREQ_HZ          400000U
#define CEEPEW_OLED_I2C_ADDR        0x3CU
#define CEEPEW_OLED_WIDTH_PX        128U
#define CEEPEW_OLED_HEIGHT_PX       64U

/* ── Rotary Potentiometer (Analog Input) — ADC1 ONLY ────────────── */
/* ADC2 is shared with WiFi and must NOT be used when WiFi/ESP-NOW is active */
#define CEEPEW_PIN_POT              GPIO_NUM_34
#define CEEPEW_ADC_CHANNEL_POT      ADC1_CHANNEL_6
#define CEEPEW_ADC_ATTEN            ADC_ATTEN_DB_11
#define CEEPEW_ADC_WIDTH            ADC_WIDTH_BIT_12

/* ── Click Button ────────────────────────────────────────────────── */
#define CEEPEW_PIN_BUTTON           GPIO_NUM_35
#define CEEPEW_BUTTON_ACTIVE_LEVEL  0

/* ── DIAG Mode Hardware Switch (push-lock) ───────────────────────── */
#define CEEPEW_PIN_DIAG_SWITCH      GPIO_NUM_5
#define CEEPEW_DIAG_SWITCH_ACTIVE   0

/* ── Status LED / RGB LED ───────────────────────────────────────── */
/* Note: GPIO 2 is commonly the on-board LED and is intentionally shared */
#define CEEPEW_PIN_STATUS_LED       GPIO_NUM_2
#define CEEPEW_LED_ACTIVE_LEVEL     1

#define CEEPEW_PIN_RGB_RED          GPIO_NUM_2
#define CEEPEW_PIN_RGB_GREEN        GPIO_NUM_18
#define CEEPEW_PIN_RGB_BLUE         GPIO_NUM_23

/* ── Radio / BLE / ESP-NOW constants (no GPIOs) ─────────────────── */
#define CEEPEW_ESPNOW_CHANNEL       1U
#define CEEPEW_ESPNOW_RATE          WIFI_PHY_RATE_MCS7_SGI
#define CEEPEW_BLE_ADV_INTERVAL_MS  100U

/* ------------------------------------------------------------------ */
/* Pin uniqueness validation — compile-time _Static_assert checks       */
/* Allowable sharing: CEEPEW_PIN_STATUS_LED == CEEPEW_PIN_RGB_RED (intentional) */
/* All other pins used as inputs/outputs must be unique.              */
#define CEEPEW_PINS_ASSERT_UNIQUE() do {                                 \
    _Static_assert(CEEPEW_PIN_I2C_SDA != CEEPEW_PIN_I2C_SCL, "I2C SDA/SCL must differ"); \
    _Static_assert(CEEPEW_PIN_I2C_SDA != CEEPEW_PIN_POT, "I2C SDA conflicts with POT"); \
    _Static_assert(CEEPEW_PIN_I2C_SCL != CEEPEW_PIN_POT, "I2C SCL conflicts with POT"); \
    _Static_assert(CEEPEW_PIN_POT != CEEPEW_PIN_BUTTON, "POT conflicts with BUTTON"); \
    _Static_assert(CEEPEW_PIN_BUTTON != CEEPEW_PIN_DIAG_SWITCH, "BUTTON conflicts with DIAG SWITCH"); \
    _Static_assert(CEEPEW_PIN_BUTTON != CEEPEW_PIN_RGB_GREEN, "BUTTON conflicts with RGB GREEN"); \
    _Static_assert(CEEPEW_PIN_BUTTON != CEEPEW_PIN_RGB_BLUE, "BUTTON conflicts with RGB BLUE"); \
    _Static_assert(CEEPEW_PIN_DIAG_SWITCH != CEEPEW_PIN_RGB_GREEN, "DIAG SWITCH conflicts with RGB GREEN"); \
    _Static_assert(CEEPEW_PIN_DIAG_SWITCH != CEEPEW_PIN_RGB_BLUE, "DIAG SWITCH conflicts with RGB BLUE"); \
    _Static_assert(CEEPEW_PIN_RGB_GREEN != CEEPEW_PIN_RGB_BLUE, "RGB pins must differ"); \
} while (0)

#endif /* HAL_PINS_H */

/* components/ceepew_oled_arduino/include/ceepew_oled_arduino_transport.h
 *
 * Pure-C interface to the Arduino Wire-based OLED transport.
 *
 * This file is the extern "C" boundary between the C++ Arduino Wire
 * implementation and the C ceepew_oled stack. No C++ types leak through.
 *
 * WHY ARDUINO WIRE:
 *   The ESP-IDF driver-ng (i2c_master_transmit) and legacy driver
 *   (i2c_master_write_to_device) both NACK on data-phase bytes for the
 *   RG0.96 IC V2.0 OLED clone. Arduino's Wire library, which uses the
 *   low-level ESP32 I2C HAL (register-level), works on the same hardware.
 *   This component provides the same register-level I2C path without
 *   requiring the full Arduino runtime by linking against Wire.h.
 *
 * License: GPL-3.0-only. See /LICENSE.
 */

#ifndef CEEPEW_OLED_ARDUINO_TRANSPORT_H
#define CEEPEW_OLED_ARDUINO_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise Wire I2C on GPIO26(SDA)/GPIO27(SCL).
 *
 * The bus clock is set to @p freq_hz (e.g. 800000 for 800 kHz).
 * Must be called once at boot, before any FreeRTOS tasks are created,
 * to avoid race conditions on the I2C peripheral. Safe to call again
 * (Wire re-init is idempotent).
 *
 * @param freq_hz  I2C clock frequency in Hz.
 */
esp_err_t ceepew_oled_arduino_init(uint32_t freq_hz);

/**
 * @brief Transmit a raw buffer to the SSD1306 over Wire.
 *
 * The buffer must already contain the SSD1306 control byte as the first
 * byte (0x00 = command stream, 0x40 = data stream, 0x80 = single command).
 * This function handles chunking for Wire's 128-byte internal TX buffer:
 * messages longer than 128 bytes are split and each chunk is re-prefixed
 * with the original control byte.
 *
 * @param data  Buffer containing control byte + payload.
 * @param len   Total length of the buffer.
 * @return ESP_OK on success, ESP_ERR_INVALID_RESPONSE on NACK.
 */
esp_err_t ceepew_oled_arduino_transmit(const uint8_t *data, size_t len);

/**
 * @brief Push a 1024-byte framebuffer to the SSD1306 GDDRAM.
 *
 * Uses Wire to write all 8 pages in page-addressing mode. Each page
 * transaction is a self-contained I2C message with the correct column
 * and page address commands followed by the page data, all chunked to
 * fit within Wire's buffer limits.
 *
 * @param framebuffer  Pointer to 1024-byte framebuffer (128 bytes x 8 pages).
 * @param size         Must be 1024 (CEEPEW_OLED_BUF_SIZE).
 * @return ESP_OK on success, ESP_ERR_INVALID_RESPONSE on NACK.
 */
esp_err_t ceepew_oled_arduino_flush(const uint8_t *framebuffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* CEEPEW_OLED_ARDUINO_TRANSPORT_H */

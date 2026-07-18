/* components/ceepew_oled_arduino/ceepew_oled_arduino_transport.cpp
 *
 * C++ implementation of the Arduino Wire-based OLED transport.
 *
 * This is the ONLY C++ translation unit in the CEE-PEW project.
 * All other files remain pure C. The extern "C" functions in
 * ceepew_oled_arduino_transport.h form the boundary.
 *
 * Wire is used instead of the ESP-IDF I2C drivers (both driver-ng
 * and legacy) because Wire accesses the I2C peripheral at the register
 * level via the ESP32 Arduino HAL, which works on the RG0.96 IC V2.0
 * OLED clone where the IDF drivers NACK.
 *
 * License: GPL-3.0-only. See /LICENSE.
 */

#include "ceepew_oled_arduino_transport.h"

#include <Wire.h>

/* ── Constants ─────────────────────────────────────────────────────── */

#define SSD1306_ADDR       0x3C
#define SSD1306_WIDTH      128
#define SSD1306_PAGES      8

/* Wire's internal TX buffer is 128 bytes (I2C_BUFFER_LENGTH on ESP32).
 * We keep one byte reserved within each transaction for a control-byte
 * re-prefix when chunking, giving a safe data-only payload of 127. */
#define WIRE_MAX_PER_TX    128U
#define WIRE_SAFE_PAYLOAD  127U

/* ── Module state ──────────────────────────────────────────────────── */

static uint8_t s_addr = SSD1306_ADDR;
static bool    s_initialised = false;

/* ── I2C transmit helper (Wire) ────────────────────────────────────── */

static esp_err_t wire_transmit(const uint8_t *data, size_t len)
{
    Wire.beginTransmission(s_addr);
    Wire.write(data, len);
    uint8_t rc = Wire.endTransmission(true);
    return (rc == 0) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/* ── Public API ─────────────────────────────────────────────────────── */

extern "C" esp_err_t ceepew_oled_arduino_init(uint32_t freq_hz)
{
    s_addr = SSD1306_ADDR;
    Wire.begin(26, 27);
    Wire.setClock(freq_hz);
    s_initialised = true;
    return ESP_OK;
}

extern "C" esp_err_t ceepew_oled_arduino_transmit(const uint8_t *data, size_t len)
{
    if (!s_initialised) { return ESP_ERR_INVALID_STATE; }
    if (data == NULL || len == 0) { return ESP_ERR_INVALID_ARG; }

    /* Single-transaction case: fits within Wire's TX buffer. */
    if (len <= WIRE_MAX_PER_TX) {
        return wire_transmit(data, len);
    }

    /* Multi-chunk case: the first chunk consumes up to WIRE_MAX_PER_TX
     * bytes including the control byte. Subsequent chunks re-prefix the
     * original control byte (data[0]) so each I2C transaction is a
     * self-contained message understood by the SSD1306. */
    const uint8_t control_byte = data[0];
    esp_err_t rc;

    rc = wire_transmit(data, WIRE_MAX_PER_TX);
    if (rc != ESP_OK) { return rc; }
    data += WIRE_MAX_PER_TX;
    len  -= WIRE_MAX_PER_TX;
    while (len > 0) {
        size_t chunk = (len > WIRE_SAFE_PAYLOAD) ? WIRE_SAFE_PAYLOAD : len;
        Wire.beginTransmission(s_addr);
        Wire.write(&control_byte, 1);
        Wire.write(data, chunk);
        uint8_t wire_rc = Wire.endTransmission(true);
        if (wire_rc != 0) { return ESP_ERR_INVALID_RESPONSE; }
        data += chunk;
        len  -= chunk;
    }

    return ESP_OK;
}

extern "C" esp_err_t ceepew_oled_arduino_flush(const uint8_t *framebuffer, size_t size)
{
    if (!s_initialised) { return ESP_ERR_INVALID_STATE; }
    if (framebuffer == NULL) { return ESP_ERR_INVALID_ARG; }
    if (size != SSD1306_WIDTH * SSD1306_PAGES) { return ESP_ERR_INVALID_SIZE; }

    for (uint8_t page = 0; page < SSD1306_PAGES; page++) {
        /* Set column start (0,0) and page address.
         * Control byte 0x00 = command stream, then set lower column,
         * higher column, and page. */
        uint8_t cmd_buf[] = {
            0x00U,
            0x00U,                         /* set lower column = 0 */
            0x10U,                         /* set higher column = 0 */
            (uint8_t)(0xB0U | page),       /* set page address */
        };
        esp_err_t rc = wire_transmit(cmd_buf, sizeof(cmd_buf));
        if (rc != ESP_OK) { return rc; }

        /* Send the 128-byte page data. Wire's buffer is 128 bytes, so
         * we split into 2 chunks: first 127 data bytes + control byte,
         * then the remaining 1 data byte + control byte reprefix. */
        const uint8_t *src = &framebuffer[(uint16_t)page * SSD1306_WIDTH];
        size_t remaining = SSD1306_WIDTH;
        const uint8_t data_ctrl = 0x40U;  /* Co=0, D/C#=1 (data stream) */

        /* Chunk 1: control byte + 127 data bytes = 128 total */
        {
            const size_t c1 = 127U;
            Wire.beginTransmission(s_addr);
            Wire.write(&data_ctrl, 1);
            Wire.write(src, c1);
            uint8_t wire_rc = Wire.endTransmission(true);
            if (wire_rc != 0) { return ESP_ERR_INVALID_RESPONSE; }
            src += c1;
            remaining -= c1;
        }

        /* Chunk 2: control byte + remaining 1 data byte = 2 total */
        if (remaining > 0) {
            Wire.beginTransmission(s_addr);
            Wire.write(&data_ctrl, 1);
            Wire.write(src, remaining);
            uint8_t wire_rc = Wire.endTransmission(true);
            if (wire_rc != 0) { return ESP_ERR_INVALID_RESPONSE; }
        }
    }

    return ESP_OK;
}

/* components/ceepew_oled/include/ceepew_oled.h
 *
 * CEE-PEW SSD1306/SH1106 OLED transport layer.
 *
 * Adafruit-style API: a single opaque device handle owns the panel's
 * configuration, I2C bus, and protocol. Callers obtain a raw pointer to
 * the 1024-byte framebuffer via ceepew_oled_get_buffer() and write
 * directly into it; ceepew_oled_display() pushes the framebuffer to the
 * panel over I2C.
 *
 * Design notes:
 *  - Framebuffer is the standard SSD1306 page layout: 8 pages x 128
 *    columns, vertical LSB-first (page = y >> 3, bit = y & 0x07,
 *    byte[x] |= (1 << bit) to set a pixel).
 *  - No dynamic allocation. The device handle, framebuffer, and any
 *    scratch state are all in the caller's static storage.
 *  - Synchronous I2C: the bus is configured with trans_queue_depth = 0
 *    so every i2c_master_transmit() blocks until the physical transfer
 *    completes.
 *  - SSD1306 vs SH1106 detection happens automatically on the first
 *    ceepew_oled_display() failure (SH1106 needs a +2 column offset
 *    in page-addressing mode).
 *  - Init follows the nopnop2002 pattern: create bus → add device
 *    (no probe) → send init stream → first transmit tests connection.
 *
 * License: GPL-3.0-only (see /LICENSE).
 */

#ifndef CEEPEW_OLED_H
#define CEEPEW_OLED_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "hal_ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CEEPEW_OLED_WIDTH_PX    128U
#define CEEPEW_OLED_HEIGHT_PX   64U
#define CEEPEW_OLED_PAGES       8U
#define CEEPEW_OLED_BUF_SIZE    (CEEPEW_OLED_WIDTH_PX * CEEPEW_OLED_PAGES)

#define CEEPEW_OLED_I2C_TIMEOUT_TICKS   200U

typedef struct ceepew_oled_t ceepew_oled_t;

/* ── Lifecycle ──────────────────────────────────────────────────── */

ceepew_oled_t *ceepew_oled_create(void);
void           ceepew_oled_destroy(ceepew_oled_t *dev);

/* ── Framebuffer access ─────────────────────────────────────────── */

uint8_t       *ceepew_oled_get_buffer(ceepew_oled_t *dev);
size_t         ceepew_oled_get_buffer_size(const ceepew_oled_t *dev);
void           ceepew_oled_clear_buffer(ceepew_oled_t *dev);
bool           ceepew_oled_get_sh1106_mode(const ceepew_oled_t *dev);

/* ── I2C bus bring-up (nopnop2002 pattern) ──────────────────────── */

/**
 * @brief Create I2C bus + add device at 0x3C. No probe.
 *        Caller owns bus lifetime.
 */
esp_err_t ceepew_oled_bus_init(i2c_master_bus_handle_t *out_bus,
                               i2c_master_dev_handle_t *out_dev,
                               gpio_num_t sda, gpio_num_t scl,
                               uint32_t speed_hz,
                               uint8_t addr);

/**
 * @brief Send SSD1306 init stream. This is the first real
 *        bus transaction — tests the connection.
 */
esp_err_t ceepew_oled_init_panel(ceepew_oled_t *dev,
                                 i2c_master_bus_handle_t bus,
                                 i2c_master_dev_handle_t dev_handle,
                                 uint8_t addr);

/**
 * @brief Send SH1106 init stream. Used as fallback when all
 *        SSD1306 init attempts fail.
 */
esp_err_t ceepew_oled_init_panel_sh1106(ceepew_oled_t *dev,
                                        i2c_master_bus_handle_t bus,
                                        i2c_master_dev_handle_t dev_handle,
                                        uint8_t addr);

/* ── Display ────────────────────────────────────────────────────── */

esp_err_t ceepew_oled_display(ceepew_oled_t *dev);
esp_err_t ceepew_oled_display_sh1106(ceepew_oled_t *dev, uint8_t col_offset);
esp_err_t ceepew_oled_push_tile(ceepew_oled_t *dev, uint8_t tile_col, uint8_t tile_row);
esp_err_t ceepew_oled_set_contrast(ceepew_oled_t *dev, uint8_t contrast);
esp_err_t ceepew_oled_set_invert(ceepew_oled_t *dev, bool invert);

/* ── Fast-mode probe (opt-in 800 kHz fallback) ──────────────────── */

bool ceepew_oled_probe_fast_mode(i2c_master_bus_handle_t bus, uint8_t addr);

#ifdef __cplusplus
}
#endif

#endif /* CEEPEW_OLED_H */

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
 * TRANSPORT: Arduino Wire (register-level I2C HAL). The IDF driver-ng
 * and legacy I2C drivers both NACK on data-phase bytes for the RG0.96
 * IC V2.0 OLED clone; the Arduino Wire transport works (see Test 007
 * in DEBUG_LOG_OLED_I2C.md). The i2c_master_bus_handle_t and
 * i2c_master_dev_handle_t types in the public API are retained as
 * sentinel handles for API compatibility with hal_ui.c.
 *
 * Design notes:
 *  - Framebuffer is the standard SSD1306 page layout: 8 pages x 128
 *    columns, vertical LSB-first (page = y >> 3, bit = y & 0x07,
 *    byte[x] |= (1 << bit) to set a pixel).
 *  - No dynamic allocation. The device handle, framebuffer, and any
 *    scratch state are all in the caller's static storage.
 *  - Synchronous I2C: every Wire.transmit() blocks until the physical
 *    transfer completes.
 *  - SSD1306 vs SH1106 selection is determined by the Kconfig option
 *    CEEPEW_OLED_FORCE_SH1106. Both panels ACK identical I2C transactions
 *    so auto-detection is unreliable — the panel type must be selected
 *    at compile time via menuconfig ("CEE-PEW OLED Configuration").
 *    SH1106 init uses 0xA0 (no segment remap) and direct column
 *    mapping — no column offset needed.
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
 * @brief Initialise the Arduino Wire I2C transport on GPIO26/27.
 *
 * The out_bus and out_dev handles are non-NULL sentinel values that
 * satisfy callers' assertion checks; they must NOT be dereferenced.
 * The actual I2C is handled entirely by the Arduino Wire transport.
 */
esp_err_t ceepew_oled_bus_init(i2c_master_bus_handle_t *out_bus,
                               i2c_master_dev_handle_t *out_dev,
                               gpio_num_t sda, gpio_num_t scl,
                               uint32_t speed_hz,
                               uint8_t addr);

/**
 * @brief Safe no-op cleanup for sentinel bus/dev handles.
 *
 * The Arduino Wire transport is initialised once at boot and persists
 * for the firmware lifetime. Do NOT call i2c_master_bus_rm_device or
 * i2c_del_master_bus on the sentinel handles.
 */
void ceepew_oled_bus_cleanup(i2c_master_bus_handle_t bus,
                              i2c_master_dev_handle_t dev);

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

/* ── Fast-mode probe (stub — not supported with Arduino Wire transport) */

bool ceepew_oled_probe_fast_mode(i2c_master_bus_handle_t bus, uint8_t addr);

/* ── Bus recovery (SCL bit-bang) and diagnostics ─────────────────── */

extern bool g_oled_in_init_stream;
void ceepew_oled_bus_recover(gpio_num_t sda, gpio_num_t scl);

#ifdef __cplusplus
}
#endif

#endif /* CEEPEW_OLED_H */

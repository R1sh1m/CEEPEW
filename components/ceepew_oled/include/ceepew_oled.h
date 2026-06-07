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
 *    completes. This matches the prior CEE-PEW driver.
 *  - SSD1306 vs SH1106 detection happens automatically on the first
 *    ceepew_oled_display() failure (SH1106 has no SET_COLUMN_RANGE/
 *    SET_PAGE_RANGE in horizontal mode and needs a +2 column offset
 *    in page-addressing mode).
 *  - CEE-PEW extensions: bus recovery (oled_bus_recover), pin-matrix
 *    bring-up (ceepew_oled_multi_attempt), and a last-ditch GPIO-pair
 *    scan (ceepew_oled_scan_all_pins) are exposed for the boot path.
 *
 * License: GPL-3.0-only (see /LICENSE).
 *
 * I2C transport patterns derived from the public-domain
 * nopnop2002/esp-idf-ssd1306 reference implementation (MIT) and adapted
 * to the new i2c_master_* API introduced in ESP-IDF v5.2 and made
 * mandatory in v6.0.
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

/* Framebuffer geometry for the 128x64 OLED. */
#define CEEPEW_OLED_WIDTH_PX    128U
#define CEEPEW_OLED_HEIGHT_PX   64U
#define CEEPEW_OLED_PAGES       8U
#define CEEPEW_OLED_BUF_SIZE    (CEEPEW_OLED_WIDTH_PX * CEEPEW_OLED_PAGES)

/* I2C transaction timeout in FreeRTOS ticks. 200 = 200 ms at the
 * project's 1 kHz tick rate. Generous on purpose; flushes must succeed
 * even when BLE has the radio busy. */
#define CEEPEW_OLED_I2C_TIMEOUT_TICKS   200U

/* Probe retries used during boot bring-up. */
#define CEEPEW_OLED_PROBE_ATTEMPTS      3U
#define CEEPEW_OLED_PROBE_RETRY_MS      50U

/* Two-speed / two-address / two-pin-pair matrix used by the boot
 * bring-up. The fallback values are tried only when the primary values
 * differ (so the matrix is 1, 2, 4, or 8 actual attempts depending on
 * the configured vs fallback pin/address/speed equality). */
typedef struct {
    gpio_num_t sda_primary;
    gpio_num_t sda_fallback;
    gpio_num_t scl_primary;
    gpio_num_t scl_fallback;
    uint32_t   speed_primary_hz;
    uint32_t   speed_fallback_hz;
    uint8_t    addr_primary;
    uint8_t    addr_fallback;
} ceepew_oled_pins_t;

/* Opaque device handle. The full struct lives in ceepew_oled.c; callers
 * must always go through the ceepew_oled_* API. */
typedef struct ceepew_oled_t ceepew_oled_t;

/* ── Lifecycle ──────────────────────────────────────────────────── */

/* Allocate and zero-initialize a new device handle. The handle owns
 * nothing at this point; no I2C transactions are issued. */
ceepew_oled_t *ceepew_oled_create(void);

/* Free a device handle. The I2C bus and device remain owned by the
 * caller; this only releases the handle's own storage. */
void ceepew_oled_destroy(ceepew_oled_t *dev);

/* Issue the standard SSD1306 init command stream over the existing
 * i2c_master_dev_handle_t. On success the panel is configured for
 * 128x64, page-addressing mode, internal charge pump on, display on.
 * Does NOT take ownership of the bus or device handle. The bus handle
 * is stored inside the device so the 800 kHz Fm+ fallback can add a
 * second i2c device to the same bus; pass NULL if 800 kHz support is
 * not desired. */
esp_err_t ceepew_oled_init_panel(ceepew_oled_t *dev,
                                 i2c_master_bus_handle_t bus,
                                 i2c_master_dev_handle_t i2c_dev,
                                 uint8_t addr);

/* Try the (pin-pair x speed x address) matrix in priority order. On
 * success, returns ESP_OK and writes the i2c_master_dev_handle_t, the
 * 7-bit address, AND the underlying bus handle through the out-params.
 * The bus handle is required for the 800 kHz fallback in
 * ceepew_oled_init_panel. SH1106 column-offset detection is NOT
 * performed here; the caller drives display() / display_sh1106 to latch
 * the mode. */
esp_err_t ceepew_oled_multi_attempt(const ceepew_oled_pins_t *pins,
                                    i2c_master_bus_handle_t *out_bus,
                                    i2c_master_dev_handle_t *out_dev,
                                    uint8_t *out_addr);

/* Last-ditch: scan every valid GPIO pair for any 0x3C/0x3D ACK. Used
 * for hardware-debugging when the configured pins fail. On success,
 * returns ESP_OK and writes the i2c_master_bus_handle_t, the
 * i2c_master_dev_handle_t, and the 7-bit address through the
 * out-params. */
esp_err_t ceepew_oled_scan_all_pins(i2c_master_bus_handle_t *out_bus,
                                    i2c_master_dev_handle_t *out_dev,
                                    uint8_t *out_addr);

/* ── Framebuffer access ─────────────────────────────────────────── */

/* Return a pointer to the device's 1024-byte framebuffer. The buffer is
 * the standard SSD1306 page layout: 8 pages x 128 columns, vertical
 * LSB-first. The pointer is valid for the lifetime of the device. */
uint8_t *ceepew_oled_get_buffer(ceepew_oled_t *dev);

/* Return the size of the framebuffer in bytes (always 1024 for the
 * 128x64 panel). */
size_t ceepew_oled_get_buffer_size(const ceepew_oled_t *dev);

/* ── Display push ──────────────────────────────────────────────── */

/* Push the current framebuffer to the panel using the SSD1306 fast
 * path: horizontal-addressing mode, 2 I2C transactions (one command
 * stream, one data stream). On failure returns ESP_FAIL; the caller
 * may retry with ceepew_oled_display_sh1106(). */
esp_err_t ceepew_oled_display(ceepew_oled_t *dev);

/* Push the current framebuffer to the panel using the SH1106 slow
 * path: page-addressing mode with a +col_offset column shift (typical
 * SH1106 panel needs +2). 8 pages x 2 I2C transactions each. */
esp_err_t ceepew_oled_display_sh1106(ceepew_oled_t *dev, uint8_t col_offset);

/* Push a single 8-column-wide x 8-row-tall tile from the framebuffer
 * to the panel. tile_col in [0, 15], tile_row in [0, 7]. The tile is
 * addressed via SET_LOWER_COL / SET_HIGHER_COL / SET_PAGE for each of
 * its 8 pages. Used by the tile-dirty fast path in hal_ui_flush. */
esp_err_t ceepew_oled_push_tile(ceepew_oled_t *dev,
                                uint8_t tile_col, uint8_t tile_row);

/* Return true if a previous ceepew_oled_display() failure was recovered
 * by switching to the SH1106 +2 column-offset path. */
bool ceepew_oled_get_sh1106_mode(const ceepew_oled_t *dev);

/* Clear the entire framebuffer to 0. */
void ceepew_oled_clear_buffer(ceepew_oled_t *dev);

/* ── Boot bring-up helpers (moved from hal_ui.c) ───────────────── */

/* Bus recovery: bit-bang SCL up to 9 times to free a stuck slave that
 * is holding SDA low. The pins are switched to open-drain output for
 * the duration of the recovery. */
void ceepew_oled_bus_recover(gpio_num_t sda_pin, gpio_num_t scl_pin);

/* Probe the given 7-bit address on the given bus, retrying up to
 * CEEPEW_OLED_PROBE_ATTEMPTS times with CEEPEW_OLED_PROBE_RETRY_MS
 * between attempts. Returns ESP_OK on ACK, ESP_FAIL otherwise. */
esp_err_t ceepew_oled_probe_with_retry(i2c_master_bus_handle_t bus, uint8_t addr);

/* Build an i2c_master_bus + i2c_master_dev pair for the given
 * (sda, scl, speed, addr) tuple. On success, returns ESP_OK and writes
 * the bus and device handles through the out-params. On any failure,
 * both out-params are set to NULL and the bus is cleaned up. */
esp_err_t ceepew_oled_bus_bringup(gpio_num_t sda, gpio_num_t scl,
                                  uint32_t speed_hz, uint8_t addr,
                                  i2c_master_bus_handle_t *out_bus,
                                  i2c_master_dev_handle_t *out_dev);

#ifdef __cplusplus
}
#endif

#endif /* CEEPEW_OLED_H */

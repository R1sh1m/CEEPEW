/* components/ceepew_hal/hal_ui.c
 *
 * UI rendering implementation: pixel/line/shape drawing,
 * font rendering, framebuffer management.
 *
 * Backing store: the vendored ssd1306 (nopnop2002/esp-idf-ssd1306) library
 * owns a static SSD1306_t whose internal _page[8]._segs[128] buffer is
 * the display framebuffer. This module writes directly into that buffer
 * and calls a no-malloc custom flush (hal_ui_flush_pages) to push all 8
 * pages over I2C in page-addressing mode. The upstream's malloc-based
 * ssd1306_show_buffer() is intentionally NOT used.
 */

#include "hal_ui.h"
#include "ssd1306.h"
#include "hal_pins.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* The vendored ssd1306.h doesn't include these by default; we use them
 * only in the hal_ui-level I2C bring-up path. */
#include "esp_err.h"

/* Forward decls from the vendored I2C transport so we can reuse the
 * same field layout without re-typing the SSD1306_t members. The struct
 * itself is fully visible in ssd1306.h. */

/* ── Module state ─────────────────────────────────────────────────── */
static SSD1306_t s_dev;             /* vendored display handle           */
static bool      s_ui_initialised;  /* hal_ui_init completed successfully */
static bool      s_sh1106_mode;     /* SH1106 needs +2 col offset         */
static bool      s_framebuffer_dirty; /* set by any drawing primitive,
                                       * cleared after a successful I2C
                                       * push. Skips redundant flushes on
                                       * idle static screens. */
static uint32_t  s_last_flush_diag_ms;
static bool      s_nonzero_flush_seen;

static const char *TAG = "hal_ui";

/* Reuse the full printable ASCII font from ui_manager.c so all UI text paths
 * render the same glyphs. The table is stored as 5 columns per character. */
extern const uint8_t s_font5x7[95][5];

/* ── I2C bring-up helpers (preserved from the previous hal_oled.c) ── */

static CeePewErr_t ssd1306_flush_pages_no_malloc(SSD1306_t *dev, uint8_t col_offset);
static void        oled_bus_recover(gpio_num_t sda_pin, gpio_num_t scl_pin);
static esp_err_t   oled_probe_with_retry(i2c_master_bus_handle_t bus, uint8_t addr);

/* I2C transaction timeout in FreeRTOS ticks. 200 = 200 ms at the project's
 * 1 kHz tick rate. Generous on purpose; we want flushes to succeed even
 * when BLE has the radio busy. */
#define I2C_FLUSH_TIMEOUT_TICKS  200U
#define OLED_PROBE_ATTEMPTS       3U
#define OLED_PROBE_RETRY_MS       50U

static esp_err_t oled_probe_with_retry(i2c_master_bus_handle_t bus, uint8_t addr)
{
    for (uint8_t attempt = 0U; attempt < OLED_PROBE_ATTEMPTS; attempt++) {
        esp_err_t rc = i2c_master_probe(bus, addr, I2C_FLUSH_TIMEOUT_TICKS);
        if (rc == ESP_OK) { return ESP_OK; }
        ESP_LOGD(TAG, "probe 0x%02X attempt %u/%u failed (%d)",
                 (unsigned)addr,
                 (unsigned)(attempt + 1U),
                 (unsigned)OLED_PROBE_ATTEMPTS,
                 (int)rc);
        if ((attempt + 1U) < OLED_PROBE_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(OLED_PROBE_RETRY_MS));
        }
    }
    return ESP_FAIL;
}

static void oled_bus_recover(gpio_num_t sda_pin, gpio_num_t scl_pin)
{
    const gpio_config_t od_cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)sda_pin) |
                        (1ULL << (uint32_t)scl_pin),
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    (void)gpio_config(&od_cfg);
    (void)gpio_set_level(sda_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(1U));

    for (uint8_t clk = 0U; clk < 9U; clk++) {
        if (gpio_get_level(sda_pin) != 0) { break; }
        (void)gpio_set_level(scl_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(1U));
        (void)gpio_set_level(scl_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(1U));
    }

    (void)gpio_set_level(scl_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(1U));
    (void)gpio_set_level(sda_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(1U));
    (void)gpio_set_level(sda_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(10U));
}

/* Build an i2c_master_bus + i2c_master_dev pair for the given pins/addr/speed.
 * On success, returns ESP_OK and the bus+dev handles are written through the
 * out-params. On any failure, both are NULL and the bus is cleaned up.
 */
static esp_err_t ssd1306_bus_bringup(gpio_num_t sda, gpio_num_t scl,
                                     uint32_t speed_hz, uint8_t addr,
                                     i2c_master_bus_handle_t *out_bus,
                                     i2c_master_dev_handle_t *out_dev)
{
    *out_bus = NULL;
    *out_dev = NULL;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port              = CEEPEW_I2C_PORT,
        .sda_io_num            = sda,
        .scl_io_num            = scl,
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt     = 7U,
        .intr_priority         = 0,
        /* Synchronous mode: no async queue, every i2c_master_transmit()
         * blocks until the physical transfer completes. Prevents the async
         * queue overflow that caused hangs in the prior driver. */
        .trans_queue_depth     = 0U,
        .flags = {
            .enable_internal_pullup = 1U,
            .allow_pd               = 0U,
        },
    };

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t rc = i2c_new_master_bus(&bus_cfg, &bus);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "i2c_new_master_bus(SDA=%d,SCL=%d,@%luHz) failed: %d (%s)",
                 (int)sda, (int)scl, (unsigned long)speed_hz,
                 (int)rc, esp_err_to_name(rc));
        return rc;
    }
    vTaskDelay(pdMS_TO_TICKS(10U));

    if (oled_probe_with_retry(bus, addr) != ESP_OK) {
        ESP_LOGW(TAG, "No ACK from 0x%02X on SDA=%d SCL=%d @%luHz after %u attempts",
                 (unsigned)addr, (int)sda, (int)scl,
                 (unsigned long)speed_hz,
                 (unsigned)OLED_PROBE_ATTEMPTS);
        (void)i2c_del_master_bus(bus);
        return ESP_FAIL;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = speed_hz,
    };
    i2c_master_dev_handle_t dev = NULL;
    rc = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "i2c_master_bus_add_device(0x%02X) failed: %d (%s)",
                 (unsigned)addr, (int)rc, esp_err_to_name(rc));
        (void)i2c_del_master_bus(bus);
        return rc;
    }

    *out_bus = bus;
    *out_dev = dev;
    return ESP_OK;
}

/* Configure the SSD1306_t struct fields and run the upstream i2c_init()
 * to send the standard SSD1306 init command stream. */
static esp_err_t ssd1306_panel_bringup(SSD1306_t *dev, uint8_t addr)
{
    dev->_address = addr;
    dev->_width   = 128;
    dev->_height  = 64;
    dev->_pages   = 8;
    dev->_flip    = false;
    dev->_i2c_num = CEEPEW_I2C_PORT;
    /* _i2c_bus_handle and _i2c_dev_handle are set by the caller just before
     * calling this function. */
    return i2c_init(dev, dev->_width, dev->_height);
}

/* Try the (pin-pair × speed × address) matrix in priority order. Returns
 * ESP_OK and populates the s_dev handles on success. SH1106 column-offset
 * detection is attempted after a successful init. */
static esp_err_t ssd1306_multi_attempt(void)
{
    const uint32_t speeds[2] = { CEEPEW_I2C_FREQ_HZ, CEEPEW_I2C_FREQ_FALLBACK_HZ };
    const uint8_t  addrs[2]  = { CEEPEW_OLED_I2C_ADDR, CEEPEW_OLED_I2C_ADDR_FB };
    const gpio_num_t sda_pins[2] = { CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SDA_FALLBACK };
    const gpio_num_t scl_pins[2] = { CEEPEW_PIN_I2C_SCL, CEEPEW_PIN_I2C_SCL_FALLBACK };

    for (uint8_t pi = 0U; pi < 2U; pi++) {
        if (pi == 1U &&
            sda_pins[1] == sda_pins[0] &&
            scl_pins[1] == scl_pins[0]) {
            continue;
        }
        if (!GPIO_IS_VALID_GPIO(sda_pins[pi]) || !GPIO_IS_VALID_GPIO(scl_pins[pi])) {
            continue;
        }

        ESP_LOGI(TAG, "Trying I2C pins SDA=%d SCL=%d",
                 (int)sda_pins[pi], (int)scl_pins[pi]);
        oled_bus_recover(sda_pins[pi], scl_pins[pi]);

        for (uint8_t si = 0U; si < 2U; si++) {
            if (si == 1U && speeds[1] == speeds[0]) { continue; }
            for (uint8_t ai = 0U; ai < 2U; ai++) {
                i2c_master_bus_handle_t bus = NULL;
                i2c_master_dev_handle_t dev = NULL;
                esp_err_t rc = ssd1306_bus_bringup(sda_pins[pi], scl_pins[pi],
                                                   speeds[si], addrs[ai],
                                                   &bus, &dev);
                if (rc != ESP_OK) { continue; }

                s_dev._i2c_bus_handle = bus;
                s_dev._i2c_dev_handle = dev;
                rc = ssd1306_panel_bringup(&s_dev, addrs[ai]);
                if (rc != ESP_OK) {
                    (void)i2c_master_bus_rm_device(dev);
                    (void)i2c_del_master_bus(bus);
                    s_dev._i2c_bus_handle = NULL;
                    s_dev._i2c_dev_handle = NULL;
                    continue;
                }

                ESP_LOGI(TAG, "SSD1306 ready at 0x%02X (SDA=%d SCL=%d @%luHz)",
                         (unsigned)addrs[ai], (int)sda_pins[pi], (int)scl_pins[pi],
                         (unsigned long)speeds[si]);
                return ESP_OK;
            }
        }
    }

    return ESP_FAIL;
}

/* Last-ditch: scan every valid GPIO pair for any 0x3C/0x3D ACK. Preserved
 * from the prior driver for hardware-debugging ergonomics. */
static esp_err_t ssd1306_scan_all_pins(void)
{
    const gpio_num_t all_pins[] = {
        GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15,
        GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_21, GPIO_NUM_22,
        GPIO_NUM_23, GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_27, GPIO_NUM_32, GPIO_NUM_33
    };
    const uint8_t addrs[2] = { CEEPEW_OLED_I2C_ADDR, CEEPEW_OLED_I2C_ADDR_FB };
    const uint8_t num_pins = sizeof(all_pins) / sizeof(all_pins[0]);

    for (uint8_t sda_idx = 0U; sda_idx < num_pins; sda_idx++) {
        for (uint8_t scl_idx = 0U; scl_idx < num_pins; scl_idx++) {
            if (sda_idx == scl_idx ||
                !GPIO_IS_VALID_GPIO(all_pins[sda_idx]) ||
                !GPIO_IS_VALID_GPIO(all_pins[scl_idx])) {
                continue;
            }
            oled_bus_recover(all_pins[sda_idx], all_pins[scl_idx]);
            for (uint8_t ai = 0U; ai < 2U; ai++) {
                i2c_master_bus_handle_t bus = NULL;
                i2c_master_dev_handle_t dev = NULL;
                esp_err_t rc = ssd1306_bus_bringup(all_pins[sda_idx], all_pins[scl_idx],
                                                   CEEPEW_I2C_FREQ_HZ, addrs[ai],
                                                   &bus, &dev);
                if (rc != ESP_OK) { continue; }

                s_dev._i2c_bus_handle = bus;
                s_dev._i2c_dev_handle = dev;
                rc = ssd1306_panel_bringup(&s_dev, addrs[ai]);
                if (rc == ESP_OK) {
                    ESP_LOGW(TAG, "*** FOUND SSD1306 at 0x%02X on SDA=%d SCL=%d ***",
                             (unsigned)addrs[ai], (int)all_pins[sda_idx],
                             (int)all_pins[scl_idx]);
                    return ESP_OK;
                }
                (void)i2c_master_bus_rm_device(dev);
                (void)i2c_del_master_bus(bus);
                s_dev._i2c_bus_handle = NULL;
                s_dev._i2c_dev_handle = NULL;
            }
        }
    }
    return ESP_FAIL;
}

/* Custom no-malloc flush. Writes all 8 pages of the upstream's internal
 * _page[]._segs[] buffer to the panel using i2c_master_transmit directly.
 *
 * Fast path (SSD1306): put the panel in horizontal-addressing mode, then
 * push the entire 1 KB framebuffer in a single I2C transaction. That's
 * 2 blocking I2C transactions per frame instead of 16 (8 pages × 2 trans
 * each) — cuts per-frame bus overhead by ~6x.
 *
 * SH1106 fallback: SH1106 does NOT support SET_COLUMN_RANGE / SET_PAGE_RANGE
 * in horizontal mode (it lacks those commands), so we keep the original
 * per-page path. col_offset is the +2 SH1106 column shift.
 */
static CeePewErr_t ssd1306_flush_pages_no_malloc(SSD1306_t *dev, uint8_t col_offset)
{
    if (dev == NULL || dev->_i2c_dev_handle == NULL) { return CEEPEW_ERR_BUSY; }

    if (col_offset == 0U) {
        /* ── SSD1306 fast path: horizontal addressing, 2 transactions ──
         *
         * The SET_PAGE_START (0xB0) command at the start pins the panel's
         * internal page counter to page 0 and resets the column pointer to
         * 0. Without this, if a previous flush had an I2C NACK mid-write
         * (leaving the page counter at some non-zero value), the next flush
         * would have its first page land at y=8*page rather than y=0 —
         * which produces the "headings pushed to the bottom" symptom.
         * Resetting the page pointer here costs 2 bytes per frame and
         * makes every flush deterministic. */
        const uint8_t cmd_stream[12U] = {
            0x00U,                                    /* CTRL_CMD                */
            0xB0U,                                    /* SET_PAGE_START — page 0  */
            0x00U,                                    /* SET_LOWER_COL — 0        */
            0x10U,                                    /* SET_HIGHER_COL — 0       */
            0x20U, 0x00U,                             /* HORIZONTAL_ADDR_MODE     */
            0x21U, 0x00U, 0x7FU,                      /* col range [0..127]       */
            0x22U, 0x00U, 0x07U,                      /* page range [0..7]        */
        };
        esp_err_t rc = i2c_master_transmit(dev->_i2c_dev_handle,
                                           cmd_stream, sizeof(cmd_stream),
                                           I2C_FLUSH_TIMEOUT_TICKS);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "horiz cmd failed: %d (%s)",
                     (int)rc, esp_err_to_name(rc));
            return CEEPEW_ERR_HW;
        }

        /* One data transaction: [0x40] + 1024 framebuffer bytes.
         * Stack-allocated, no malloc, fits in 4 KB UI task stack with room
         * to spare. */
        uint8_t data_stream[1U + 1024U];
        data_stream[0U] = 0x40U;
        for (uint8_t page = 0U; page < 8U; page++) {
            memcpy(&data_stream[1U + (uint16_t)page * 128U],
                   dev->_page[page]._segs, 128U);
        }
        rc = i2c_master_transmit(dev->_i2c_dev_handle,
                                 data_stream, sizeof(data_stream),
                                 I2C_FLUSH_TIMEOUT_TICKS);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "horiz data failed: %d (%s)",
                     (int)rc, esp_err_to_name(rc));
            return CEEPEW_ERR_HW;
        }
        return CEEPEW_OK;
    }

    /* ── SH1106 (or any panel needing column offset) fallback ── */
    for (uint8_t page = 0U; page < 8U; page++) {
        const uint8_t col_low  = (uint8_t)(col_offset & 0x0FU);
        const uint8_t col_high = (uint8_t)(0x10U | ((col_offset >> 4U) & 0x0FU));
        const uint8_t page_cmd = (uint8_t)(0xB0U | page);

        const uint8_t cmd_stream[4] = {
            0x00U,         /* CTRL_CMD = command stream */
            col_low,
            col_high,
            page_cmd,
        };
        esp_err_t rc = i2c_master_transmit(dev->_i2c_dev_handle,
                                           cmd_stream, sizeof(cmd_stream),
                                           I2C_FLUSH_TIMEOUT_TICKS);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "page %u cmd failed: %d (%s)",
                     (unsigned)page, (int)rc, esp_err_to_name(rc));
            return CEEPEW_ERR_HW;
        }

        uint8_t data_stream[1U + 128U];
        data_stream[0U] = 0x40U;
        memcpy(&data_stream[1U], dev->_page[page]._segs, 128U);
        rc = i2c_master_transmit(dev->_i2c_dev_handle,
                                 data_stream, sizeof(data_stream),
                                 I2C_FLUSH_TIMEOUT_TICKS);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "page %u data failed: %d (%s)",
                     (unsigned)page, (int)rc, esp_err_to_name(rc));
            return CEEPEW_ERR_HW;
        }
    }
    return CEEPEW_OK;
}

/* ── Public API ───────────────────────────────────────────────────── */

CeePewErr_t hal_ui_init(void)
{
    CEEPEW_ASSERT(!s_ui_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SDA) &&
                  GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SCL), CEEPEW_ERR_PINS);

    ESP_LOGI(TAG, "hal_ui_init (vendored ssd1306 driver, I2C only)");
    memset(&s_dev, 0, sizeof(s_dev));
    s_sh1106_mode = false;
    s_framebuffer_dirty = true;   /* initial flush below is non-redundant */
    s_nonzero_flush_seen = false;
    s_last_flush_diag_ms = 0U;

    esp_err_t rc = ssd1306_multi_attempt();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Configured pins failed; scanning all GPIO pairs");
        rc = ssd1306_scan_all_pins();
    }
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "No SSD1306 found anywhere; hal_ui_init failed");
        return CEEPEW_ERR_HW;
    }

    /* Zero the internal page buffer so a first flush shows a clean screen
     * even if the panel retained content from a prior power-on. */
    for (uint8_t p = 0U; p < 8U; p++) {
        memset(s_dev._page[p]._segs, 0, 128U);
    }

    s_ui_initialised = true;

    CeePewErr_t flush_err = ssd1306_flush_pages_no_malloc(&s_dev, 0U);
    if (flush_err != CEEPEW_OK) {
        ESP_LOGE(TAG, "Initial flush failed");
        s_ui_initialised = false;
        return flush_err;
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_clear(void)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    for (uint8_t p = 0U; p < 8U; p++) {
        memset(s_dev._page[p]._segs, 0, 128U);
    }
    s_framebuffer_dirty = true;
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_flush(void)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);

    /* Skip the I2C push entirely if nothing changed since the last flush.
     * Static screens (info, fingerprint-confirm, error, …) hit this path
     * every tick after the first draw, leaving the bus free for BLE. */
    if (!s_framebuffer_dirty) {
        return CEEPEW_OK;
    }

    /* Diagnostic: count non-zero bytes (mirrors prior behavior) */
    uint32_t nonzero_bytes = 0U;
    uint8_t  first_nonzero_idx = 0U;
    bool first_set = false;
    for (uint8_t p = 0U; p < 8U; p++) {
        for (uint16_t i = 0U; i < 128U; i++) {
            if (s_dev._page[p]._segs[i] != 0U) {
                nonzero_bytes++;
                if (!first_set) {
                    first_nonzero_idx = (uint8_t)((p << 4U) | (i & 0x0FU));
                    first_set = true;
                }
            }
        }
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    if ((!s_nonzero_flush_seen && (nonzero_bytes > 0U)) ||
        ((now_ms - s_last_flush_diag_ms) > 5000U)) {
        ESP_LOGI(TAG, "flush diag: nonzero=%lu first_idx=0x%02X",
                 (unsigned long)nonzero_bytes,
                 (unsigned int)first_nonzero_idx);
        s_last_flush_diag_ms = now_ms;
        if (nonzero_bytes > 0U) { s_nonzero_flush_seen = true; }
    }

    /* Try SSD1306 (col offset 0) first; on failure retry with SH1106 +2
     * offset and latch the mode. This mirrors the previous driver's
     * auto-detection behavior. */
    CeePewErr_t err = ssd1306_flush_pages_no_malloc(&s_dev, 0U);
    if (err != CEEPEW_OK && !s_sh1106_mode) {
        err = ssd1306_flush_pages_no_malloc(&s_dev, 2U);
        if (err == CEEPEW_OK) {
            s_sh1106_mode = true;
            ESP_LOGW(TAG, "Switched to SH1106 +2 column offset");
        } else {
            ESP_LOGE(TAG, "hal_ui_flush failed on both SSD1306 and SH1106 attempts");
        }
    }
    /* Only clear the dirty flag on a successful push so a transient I2C
     * failure still gets retried on the next tick. */
    if (err == CEEPEW_OK) {
        s_framebuffer_dirty = false;
    }
    return err;
}

SSD1306_t *hal_ui_get_dev(void)
{
    return s_ui_initialised ? &s_dev : NULL;
}

static inline void fb_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= HAL_UI_WIDTH_PX || y >= HAL_UI_HEIGHT_PX) { return; }
    const uint8_t  page = (uint8_t)(y >> 3);
    const uint8_t  bit  = (uint8_t)(1U << (y & 0x07U));
    if (on) { s_dev._page[page]._segs[x] |= bit; }
    else    { s_dev._page[page]._segs[x] = (uint8_t)(s_dev._page[page]._segs[x] & ~bit); }
    s_framebuffer_dirty = true;
}

CeePewErr_t hal_ui_pixel(uint8_t x, uint8_t y, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(x < HAL_UI_WIDTH_PX && y < HAL_UI_HEIGHT_PX, CEEPEW_ERR_BOUNDS);

    switch (color) {
        case HAL_UI_WHITE:  fb_pixel(x, y, true);  break;
        case HAL_UI_BLACK:  fb_pixel(x, y, false); break;
        case HAL_UI_INVERT: {
            const uint8_t page = (uint8_t)(y >> 3);
            const uint8_t bit  = (uint8_t)(1U << (y & 0x07U));
            s_dev._page[page]._segs[x] ^= bit;
            s_framebuffer_dirty = true;
            break;
        }
        default: return CEEPEW_ERR_PARAM;
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_hline(uint8_t x0, uint8_t x1, uint8_t y, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(y < HAL_UI_HEIGHT_PX, CEEPEW_ERR_BOUNDS);

    if (x0 > x1) { uint8_t tmp = x0; x0 = x1; x1 = tmp; }
    for (uint8_t x = x0; x <= x1 && x < HAL_UI_WIDTH_PX; x++) {
        (void)hal_ui_pixel(x, y, color);
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_vline(uint8_t x, uint8_t y0, uint8_t y1, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(x < HAL_UI_WIDTH_PX, CEEPEW_ERR_BOUNDS);

    if (y0 > y1) { uint8_t tmp = y0; y0 = y1; y1 = tmp; }
    for (uint8_t y = y0; y <= y1 && y < HAL_UI_HEIGHT_PX; y++) {
        (void)hal_ui_pixel(x, y, color);
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_line(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(x0 < HAL_UI_WIDTH_PX && y0 < HAL_UI_HEIGHT_PX, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(x1 < HAL_UI_WIDTH_PX && y1 < HAL_UI_HEIGHT_PX, CEEPEW_ERR_BOUNDS);

    int dx = (int)x1 - (int)x0;
    int dy = (int)y1 - (int)y0;
    int sx = (dx >= 0) ? 1 : -1;
    int sy = (dy >= 0) ? 1 : -1;

    dx = (dx < 0) ? -dx : dx;
    dy = (dy < 0) ? -dy : dy;

    int err = (dx > dy) ? (dx / 2) : -(dy / 2);
    int x = (int)x0;
    int y = (int)y0;

    while (true) {
        if (x >= 0 && x < (int)HAL_UI_WIDTH_PX && y >= 0 && y < (int)HAL_UI_HEIGHT_PX) {
            (void)hal_ui_pixel((uint8_t)x, (uint8_t)y, color);
        }
        if (x == (int)x1 && y == (int)y1) { break; }
        int e2 = err;
        if (e2 > -dx) { err = e2 - dy; x += sx; }
        if (e2 <  dy) { err = e2 + dx; y += sy; }
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_rect(const HalUIRect_t *r, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r->x + r->w <= HAL_UI_WIDTH_PX && r->y + r->h <= HAL_UI_HEIGHT_PX,
                  CEEPEW_ERR_BOUNDS);

    (void)hal_ui_hline(r->x, (uint8_t)(r->x + r->w - 1U), r->y, color);
    (void)hal_ui_hline(r->x, (uint8_t)(r->x + r->w - 1U), (uint8_t)(r->y + r->h - 1U), color);
    (void)hal_ui_vline(r->x, r->y, (uint8_t)(r->y + r->h - 1U), color);
    (void)hal_ui_vline((uint8_t)(r->x + r->w - 1U), r->y, (uint8_t)(r->y + r->h - 1U), color);
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_rect_fill(const HalUIRect_t *r, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r->x + r->w <= HAL_UI_WIDTH_PX && r->y + r->h <= HAL_UI_HEIGHT_PX,
                  CEEPEW_ERR_BOUNDS);

    for (uint8_t y = 0U; y < r->h; y++) {
        (void)hal_ui_hline(r->x, (uint8_t)(r->x + r->w - 1U), (uint8_t)(r->y + y), color);
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_circle(uint8_t cx, uint8_t cy, uint8_t radius, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);

    int x = (int)radius;
    int y = 0;
    int err = 0;

    while (x >= y) {
        if ((int)cx + x < (int)HAL_UI_WIDTH_PX && (int)cy + y < (int)HAL_UI_HEIGHT_PX) {
            (void)hal_ui_pixel((uint8_t)(cx + x), (uint8_t)(cy + y), color);
        }
        if ((int)cx - x >= 0 && (int)cy + y < (int)HAL_UI_HEIGHT_PX) {
            (void)hal_ui_pixel((uint8_t)(cx - x), (uint8_t)(cy + y), color);
        }
        if ((int)cx + y < (int)HAL_UI_WIDTH_PX && (int)cy + x < (int)HAL_UI_HEIGHT_PX) {
            (void)hal_ui_pixel((uint8_t)(cx + y), (uint8_t)(cy + x), color);
        }
        if ((int)cx - y >= 0 && (int)cy + x < (int)HAL_UI_HEIGHT_PX) {
            (void)hal_ui_pixel((uint8_t)(cx - y), (uint8_t)(cy + x), color);
        }
        if ((int)cx + x < (int)HAL_UI_WIDTH_PX && (int)cy - y >= 0) {
            (void)hal_ui_pixel((uint8_t)(cx + x), (uint8_t)(cy - y), color);
        }
        if ((int)cx - x >= 0 && (int)cy - y >= 0) {
            (void)hal_ui_pixel((uint8_t)(cx - x), (uint8_t)(cy - y), color);
        }
        if ((int)cx + y < (int)HAL_UI_WIDTH_PX && (int)cy - x >= 0) {
            (void)hal_ui_pixel((uint8_t)(cx + y), (uint8_t)(cy - x), color);
        }
        if ((int)cx - y >= 0 && (int)cy - x >= 0) {
            (void)hal_ui_pixel((uint8_t)(cx - y), (uint8_t)(cy - x), color);
        }
        y++;
        err += 2U * y + 1U;
        if (2U * err - (2U * x - 1U) > 0) {
            x--;
            err += 1U - 2U * x;
        }
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_circle_fill(uint8_t cx, uint8_t cy, uint8_t radius, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);

    int x = (int)radius;
    int y = 0;
    int err = 0;

    while (x >= y) {
        if ((int)cx + x < (int)HAL_UI_WIDTH_PX && (int)cy + y < (int)HAL_UI_HEIGHT_PX) {
            (void)hal_ui_hline((uint8_t)(cx - x), (uint8_t)(cx + x), (uint8_t)(cy + y), color);
        }
        if ((int)cy - y >= 0) {
            (void)hal_ui_hline((uint8_t)(cx - x), (uint8_t)(cx + x), (uint8_t)(cy - y), color);
        }
        if ((int)cx + y < (int)HAL_UI_WIDTH_PX) {
            (void)hal_ui_hline((uint8_t)(cx - y), (uint8_t)(cx + y), (uint8_t)(cy + x), color);
        }
        if ((int)cy - x >= 0) {
            (void)hal_ui_hline((uint8_t)(cx - y), (uint8_t)(cx + y), (uint8_t)(cy - x), color);
        }
        y++;
        err += 2U * y + 1U;
        if (2U * err - (2U * x - 1U) > 0) {
            x--;
            err += 1U - 2U * x;
        }
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_text(uint8_t x, uint8_t y, const char *str, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(str != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t cx = x;
    for (uint16_t i = 0U; str[i] != '\0' && cx < HAL_UI_WIDTH_PX; i++) {
        (void)hal_ui_char(cx, y, str[i], color);
        cx = (uint8_t)(cx + 6U);
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_char(uint8_t x, uint8_t y, char c, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);

    uint8_t idx;
    if ((uint8_t)c < 32U || (uint8_t)c > 126U) {
        idx = (uint8_t)('?' - 32U);
    } else {
        idx = (uint8_t)((uint8_t)c - 32U);
    }

    for (uint8_t col = 0U; col < 5U; col++) {
        uint8_t col_data = s_font5x7[idx][col];
        for (uint8_t row = 0U; row < 7U; row++) {
            if (((col_data >> row) & 1U) != 0U) {
                uint8_t px = (uint8_t)(x + col);
                uint8_t py = (uint8_t)(y + row);
                if (px < HAL_UI_WIDTH_PX && py < HAL_UI_HEIGHT_PX) {
                    (void)hal_ui_pixel(px, py, color);
                }
            }
        }
    }
    return CEEPEW_OK;
}

uint16_t hal_ui_text_width(const char *str)
{
    if (str == NULL) { return 0U; }
    uint16_t width = 0U;
    for (uint16_t i = 0U; str[i] != '\0' && i < 128U; i++) {
        width = (uint16_t)(width + 6U);
    }
    return width;
}

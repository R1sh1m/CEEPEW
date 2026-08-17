/* components/ceepew_hal/hal_ui.c
 *
 * UI rendering implementation: pixel/line/shape drawing,
 * font rendering, framebuffer management.
 *
 * Backing store: the in-house ceepew_oled component (see
 * components/ceepew_oled/) owns a 1024-byte framebuffer in the device
 * struct. This module writes directly into that buffer and calls
 * ceepew_oled_display() / ceepew_oled_display_sh1106() to push it
 * to the panel over I2C.
 *
 * The 5×7 monospace font is in ui_manager.c (s_font5x7[95][5]); we
 * read from it via extern.
 */

#include "hal_ui.h"
#include "ceepew_oled.h"
#include "ceepew_oled_gfx_primitives.h"
#include "hal_pins.h"
#include "esp_err.h"
#include "ceepew_assert.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "esp_mac.h"

static uint8_t get_board_tag(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        return mac[5];
    }
    return 0;
}

/* ── Module state ─────────────────────────────────────────────────── */
static ceepew_oled_t *s_oled;       /* the panel device handle           */
static bool           s_ui_initialised;  /* hal_ui_init completed    */
static bool           s_sh1106_mode;     /* SH1106 +2 col offset    */
static bool           s_framebuffer_dirty; /* fast-path: anything dirty? */
static bool           s_display_absent;       /* true → no OLED connected, all ops are no-ops */
static uint32_t       s_last_flush_diag_ms;
static bool           s_nonzero_flush_seen;

/* We keep a pointer to the panel's framebuffer locally. The pointer is
 * constant for the lifetime of the panel (ceepew_oled_get_buffer
 * returns the same address), so caching it here is safe. */
static uint8_t       *s_fb;

/* Tile-dirty bitmap: 16 column-tiles (128/8) x 1 byte per tile-row
 * (8 rows packed into a single byte per column-tile). When any pixel
 * in a tile is touched, the corresponding bit is set. The 16-byte
 * bitmap is walked by hal_ui_flush to push only the dirty tiles. The
 * fast-path "anything dirty" check is `s_framebuffer_dirty`, which
 * is set whenever any bit in the bitmap transitions 0->1. */
#define HAL_UI_NUM_TILE_COLS 16U
static uint8_t        s_tile_dirty[HAL_UI_NUM_TILE_COLS];

static const char *TAG = "hal_ui";

/* Reuse the full printable ASCII font from ui_manager.c so all UI text
 * paths render the same glyphs. The table is stored as 5 columns per
 * character. */
extern const uint8_t s_font5x7[95][5];

/* ── Forward declarations ─────────────────────────────────────────── */
static CeePewErr_t ssd1306_bringup(void);
static CeePewErr_t ssd1306_display_push(void);
static CeePewErr_t ssd1306_display_push_tiled(void);

/* ── Tile-dirty helpers ───────────────────────────────────────────── */

static inline void hal_ui_mark_tile_dirty(uint8_t x, uint8_t y)
{
    /* Each column-tile covers x in [tc*8, tc*8+7]. Each row bit
     * covers y in [0..7]. */
    const uint8_t tc = (uint8_t)(x >> 3U);
    const uint8_t tr = (uint8_t)(y >> 3U);
    const uint8_t mask = (uint8_t)(1U << tr);
    if ((s_tile_dirty[tc] & mask) == 0U) {
        s_tile_dirty[tc] = (uint8_t)(s_tile_dirty[tc] | mask);
        s_framebuffer_dirty = true;
    }
}

static void hal_ui_clear_tile_dirty(void)
{
    (void)memset(s_tile_dirty, 0, sizeof(s_tile_dirty));
    s_framebuffer_dirty = false;
}

static inline void hal_ui_mark_tiles_in_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    if (w == 0U || h == 0U) { return; }
    const uint8_t tc0 = (uint8_t)(x >> 3U);
    const uint8_t tc1 = (uint8_t)((uint8_t)(x + w - 1U) >> 3U);
    const uint8_t tr0 = (uint8_t)(y >> 3U);
    const uint8_t tr1 = (uint8_t)((uint8_t)(y + h - 1U) >> 3U);
    for (uint8_t tc = tc0; tc <= tc1 && tc < HAL_UI_NUM_TILE_COLS; tc++) {
        for (uint8_t tr = tr0; tr <= tr1 && tr < 8U; tr++) {
            const uint8_t mask = (uint8_t)(1U << tr);
            if ((s_tile_dirty[tc] & mask) == 0U) {
                s_tile_dirty[tc] = (uint8_t)(s_tile_dirty[tc] | mask);
                s_framebuffer_dirty = true;
            }
        }
    }
}

/* ── Boot bring-up ────────────────────────────────────────────────── */

static CeePewErr_t try_bringup_config(gpio_num_t sda, gpio_num_t scl, uint32_t freq, uint8_t addr)
{
    static bool s_first_probe_done = false;
    if (!s_first_probe_done) {
        /* Small settle delay for the I2C peripheral on the very first probe.
         * The hal_ui_init already has a 500ms delay, but the OLED panel may
         * need a brief additional settling period after the bus is created. */
        vTaskDelay(pdMS_TO_TICKS(50U));
        s_first_probe_done = true;
    }
    /* Force digital IOMUX on SDA/SCL to clear any RTC domain mux conflict
     * (GPIO 26/27 are RTC-domain on ESP32 — the RTC peripheral may hold
     * them in analog mode after WiFi/BT init). */
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t rc = ceepew_oled_bus_init(&bus, &dev, sda, scl, freq, addr);
    if (rc != ESP_OK) {
        return CEEPEW_ERR_HW;
    }

    s_oled = ceepew_oled_create();
    if (s_oled == NULL) {
        (void)ceepew_oled_bus_cleanup(bus, dev);
        return CEEPEW_ERR_HW;
    }
    s_fb = ceepew_oled_get_buffer(s_oled);

    rc = ceepew_oled_init_panel(s_oled, bus, dev, addr);
    if (rc == ESP_OK) {
        s_sh1106_mode = ceepew_oled_get_sh1106_mode(s_oled);
        ESP_LOGI(TAG, "OLED bring-up SUCCESS (%s) on SDA=%d SCL=%d Addr=0x%02X Freq=%luHz",
                 s_sh1106_mode ? "SH1106" : "SSD1306",
                 (int)sda, (int)scl, (unsigned)addr, (unsigned long)freq);
        return CEEPEW_OK;
    }

    ESP_LOGW(TAG, "OLED bring-up failed on SDA=%d SCL=%d Addr=0x%02X Freq=%luHz: %d (%s)",
             (int)sda, (int)scl, (unsigned)addr, (unsigned long)freq, (int)rc, esp_err_to_name(rc));

    /* Clean up the failed attempt */
    (void)ceepew_oled_bus_cleanup(bus, dev);
    ceepew_oled_destroy(s_oled);
    s_oled = NULL;
    s_fb = NULL;

    return CEEPEW_ERR_HW;
}

static CeePewErr_t try_bringup_config_sh1106(gpio_num_t sda, gpio_num_t scl, uint32_t freq, uint8_t addr)
{
    static bool s_first_probe_done_sh1106 = false;
    if (!s_first_probe_done_sh1106) {
        vTaskDelay(pdMS_TO_TICKS(50U));
        s_first_probe_done_sh1106 = true;
    }
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t rc = ceepew_oled_bus_init(&bus, &dev, sda, scl, freq, addr);
    if (rc != ESP_OK) {
        return CEEPEW_ERR_HW;
    }

    s_oled = ceepew_oled_create();
    if (s_oled == NULL) {
        (void)ceepew_oled_bus_cleanup(bus, dev);
        return CEEPEW_ERR_HW;
    }
    s_fb = ceepew_oled_get_buffer(s_oled);

    rc = ceepew_oled_init_panel_sh1106(s_oled, bus, dev, addr);
    if (rc == ESP_OK) {
        s_sh1106_mode = ceepew_oled_get_sh1106_mode(s_oled);
        ESP_LOGI(TAG, "OLED bring-up SUCCESS (SH1106) on SDA=%d SCL=%d Addr=0x%02X Freq=%luHz",
                 (int)sda, (int)scl, (unsigned)addr, (unsigned long)freq);
        return CEEPEW_OK;
    }

    ESP_LOGW(TAG, "OLED bring-up failed on SDA=%d SCL=%d Addr=0x%02X Freq=%luHz: %d (%s)",
             (int)sda, (int)scl, (unsigned)addr, (unsigned long)freq, (int)rc, esp_err_to_name(rc));

    /* Clean up the failed attempt */
    (void)ceepew_oled_bus_cleanup(bus, dev);
    ceepew_oled_destroy(s_oled);
    s_oled = NULL;
    s_fb = NULL;

    return CEEPEW_ERR_HW;
}

static CeePewErr_t ssd1306_bringup(void)
{
    /* Try SSD1306 configurations first */
    if (try_bringup_config(CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL, CEEPEW_I2C_FREQ_HZ, CEEPEW_OLED_I2C_ADDR) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config(CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL, CEEPEW_I2C_FREQ_HZ, CEEPEW_OLED_I2C_ADDR_FB) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config(CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL, CEEPEW_I2C_FREQ_FALLBACK_HZ, CEEPEW_OLED_I2C_ADDR) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config(CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL, CEEPEW_I2C_FREQ_FALLBACK_HZ, CEEPEW_OLED_I2C_ADDR_FB) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config(CEEPEW_PIN_I2C_SDA_FALLBACK, CEEPEW_PIN_I2C_SCL_FALLBACK, CEEPEW_I2C_FREQ_HZ, CEEPEW_OLED_I2C_ADDR) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config(CEEPEW_PIN_I2C_SDA_FALLBACK, CEEPEW_PIN_I2C_SCL_FALLBACK, CEEPEW_I2C_FREQ_HZ, CEEPEW_OLED_I2C_ADDR_FB) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config(CEEPEW_PIN_I2C_SDA_FALLBACK, CEEPEW_PIN_I2C_SCL_FALLBACK, CEEPEW_I2C_FREQ_FALLBACK_HZ, CEEPEW_OLED_I2C_ADDR) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config(CEEPEW_PIN_I2C_SDA_FALLBACK, CEEPEW_PIN_I2C_SCL_FALLBACK, CEEPEW_I2C_FREQ_FALLBACK_HZ, CEEPEW_OLED_I2C_ADDR_FB) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    /* Fallback to SH1106 configurations second */
    ESP_LOGW(TAG, "All SSD1306 configs failed, trying SH1106 init...");

    if (try_bringup_config_sh1106(CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL, CEEPEW_I2C_FREQ_HZ, CEEPEW_OLED_I2C_ADDR) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config_sh1106(CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL, CEEPEW_I2C_FREQ_HZ, CEEPEW_OLED_I2C_ADDR_FB) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config_sh1106(CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL, CEEPEW_I2C_FREQ_FALLBACK_HZ, CEEPEW_OLED_I2C_ADDR) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config_sh1106(CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL, CEEPEW_I2C_FREQ_FALLBACK_HZ, CEEPEW_OLED_I2C_ADDR_FB) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config_sh1106(CEEPEW_PIN_I2C_SDA_FALLBACK, CEEPEW_PIN_I2C_SCL_FALLBACK, CEEPEW_I2C_FREQ_HZ, CEEPEW_OLED_I2C_ADDR) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config_sh1106(CEEPEW_PIN_I2C_SDA_FALLBACK, CEEPEW_PIN_I2C_SCL_FALLBACK, CEEPEW_I2C_FREQ_HZ, CEEPEW_OLED_I2C_ADDR_FB) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config_sh1106(CEEPEW_PIN_I2C_SDA_FALLBACK, CEEPEW_PIN_I2C_SCL_FALLBACK, CEEPEW_I2C_FREQ_FALLBACK_HZ, CEEPEW_OLED_I2C_ADDR) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    if (try_bringup_config_sh1106(CEEPEW_PIN_I2C_SDA_FALLBACK, CEEPEW_PIN_I2C_SCL_FALLBACK, CEEPEW_I2C_FREQ_FALLBACK_HZ, CEEPEW_OLED_I2C_ADDR_FB) == CEEPEW_OK) {
        return CEEPEW_OK;
    }

    ESP_LOGE(TAG, "All OLED bring-up configurations failed (SSD1306 + SH1106)!");
    return CEEPEW_ERR_HW;
}

static CeePewErr_t ssd1306_display_push(void)
{
    esp_err_t rc;
    rc = ceepew_oled_display(s_oled);
    if (rc != ESP_OK) {
        rc = ceepew_oled_display_sh1106(s_oled, 0U);
        if (rc == ESP_OK) {
            s_sh1106_mode = true;
            ESP_LOGW(TAG, "Fallback SH1106 init OK");
        } else {
            ESP_LOGE(TAG, "display push failed on both SSD1306 and SH1106 attempts");
        }
    }
    if (rc == ESP_OK) {
        hal_ui_clear_tile_dirty();
    }
    return (rc == ESP_OK) ? CEEPEW_OK : CEEPEW_ERR_HW;
}

/* Tile-dirty push: walks the 16-byte bitmap and pushes only the tiles
 * that have any pixel set, then clears the bitmap. If more than 8
 * tiles are dirty, this is slower than a single full-frame push, so
 * hal_ui_flush() falls back to ssd1306_display_push() in that case. */
static CeePewErr_t ssd1306_display_push_tiled(void)
{
    uint8_t tiles_pushed = 0U;
    for (uint8_t tc = 0U; tc < HAL_UI_NUM_TILE_COLS; tc++) {
        const uint8_t rows = s_tile_dirty[tc];
        if (rows == 0U) { continue; }
        for (uint8_t tr = 0U; tr < 8U; tr++) {
            if ((rows & (uint8_t)(1U << tr)) == 0U) { continue; }
            esp_err_t rc = ceepew_oled_push_tile(s_oled, tc, tr);
            if (rc != ESP_OK) {
                ESP_LOGE(TAG, "push_tile failed at tc=%u tr=%u: %d (%s)",
                         (unsigned)tc, (unsigned)tr,
                         (int)rc, esp_err_to_name(rc));
                return CEEPEW_ERR_HW;
            }
            tiles_pushed++;
        }
    }
    hal_ui_clear_tile_dirty();
    ESP_LOGD(TAG, "tiled push: %u tile(s)", (unsigned)tiles_pushed);
    return CEEPEW_OK;
}

/* ── Public API ───────────────────────────────────────────────────── */

CeePewErr_t hal_ui_init(void)
{
    CEEPEW_ASSERT(!s_ui_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SDA) &&
                  GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SCL), CEEPEW_ERR_PINS);

    /* I2C peripheral settling delay.
     *
     * The boot-time I2C scanner (hal_i2c_scanner.c) creates and destroys the
     * I2C master bus on port 0 (GPIO26/27) before this function runs. After
     * i2c_del_master_bus(), the ESP32 I2C0 peripheral hardware enters a reset
     * sequence that takes ~50–75 ms to complete. During that window, a new bus
     * on port 0 can be created at the driver level (returns ESP_OK) but the
     * peripheral cannot clock out data bytes — every i2c_master_transmit()
     * returns ESP_ERR_INVALID_RESPONSE (264) because the device NACKs.
     *
     * 100 ms covers the peripheral reset with margin. The OLED panel itself
     * powers up with the board; its internal reset is handled by the init
     * sequence. In production builds where the scanner is gated behind
     * CONFIG_CEEPEW_DEVELOPMENT_MODE, this delay provides a brief power-up
     * margin for the OLED controller. */
    vTaskDelay(pdMS_TO_TICKS(100U));

    ESP_LOGI(TAG, "[BOARD %02X] hal_ui_init (in-house ceepew_oled, I2C only)", get_board_tag());
    s_sh1106_mode        = false;
    s_framebuffer_dirty   = true;   /* initial flush below is non-redundant */
    s_display_absent      = false;
    s_nonzero_flush_seen  = false;
    s_last_flush_diag_ms  = 0U;

    g_oled_in_init_stream = true;
    int64_t init_start_time_us = esp_timer_get_time();

    ESP_LOGI(TAG, "[BOARD %02X] OLED Init Attempt 1: Normal init (Primary Config)", get_board_tag());
    CeePewErr_t err = try_bringup_config(CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL, CEEPEW_I2C_FREQ_HZ, CEEPEW_OLED_I2C_ADDR);
    if (err == CEEPEW_OK) {
        int64_t elapsed_ms = (esp_timer_get_time() - init_start_time_us) / 1000;
        ESP_LOGI(TAG, "[BOARD %02X] OLED Init Attempt 1 SUCCESS. Elapsed time: %lld ms", get_board_tag(), elapsed_ms);
    } else {
        int64_t elapsed_ms = (esp_timer_get_time() - init_start_time_us) / 1000;
        ESP_LOGW(TAG, "[BOARD %02X] OLED Init Attempt 1 FAILED. Elapsed time: %lld ms", get_board_tag(), elapsed_ms);

        ESP_LOGI(TAG, "[BOARD %02X] Attempting bus recovery (SCL bit-bang)", get_board_tag());
        ceepew_oled_bus_recover(CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL);

        ESP_LOGI(TAG, "[BOARD %02X] OLED Init Attempt 2: Retry init after bus recovery", get_board_tag());
        err = try_bringup_config(CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL, CEEPEW_I2C_FREQ_HZ, CEEPEW_OLED_I2C_ADDR);
        if (err == CEEPEW_OK) {
            elapsed_ms = (esp_timer_get_time() - init_start_time_us) / 1000;
            ESP_LOGI(TAG, "[BOARD %02X] OLED Init Attempt 2 SUCCESS. Elapsed time: %lld ms", get_board_tag(), elapsed_ms);
        } else {
            elapsed_ms = (esp_timer_get_time() - init_start_time_us) / 1000;
            ESP_LOGW(TAG, "[BOARD %02X] OLED Init Attempt 2 FAILED. Elapsed time: %lld ms", get_board_tag(), elapsed_ms);

            ESP_LOGI(TAG, "[BOARD %02X] Attempting full multi-attempt pin/speed/address matrix", get_board_tag());
            err = ssd1306_bringup();
            if (err == CEEPEW_OK) {
                elapsed_ms = (esp_timer_get_time() - init_start_time_us) / 1000;
                ESP_LOGI(TAG, "[BOARD %02X] OLED Init Attempt 3 SUCCESS. Elapsed time: %lld ms", get_board_tag(), elapsed_ms);
            } else {
                elapsed_ms = (esp_timer_get_time() - init_start_time_us) / 1000;
                ESP_LOGE(TAG, "[BOARD %02X] OLED Init Attempt 3 FAILED. Elapsed time: %lld ms", get_board_tag(), elapsed_ms);
            }
        }
    }

    g_oled_in_init_stream = false;

    if (err != CEEPEW_OK) {
        ESP_LOGW(TAG, "[BOARD %02X] OLED init failed — continuing headless; all display ops are no-ops", get_board_tag());
        s_display_absent    = true;
        s_ui_initialised    = true;
        return CEEPEW_OK;
    }

    /* Zero the framebuffer so a first flush shows a clean screen even
     * if the panel retained content from a prior power-on. */
    (void)memset(s_fb, 0, ceepew_oled_get_buffer_size(s_oled));

    s_ui_initialised = true;

    CeePewErr_t flush_err = ssd1306_display_push();
    if (flush_err != CEEPEW_OK) {
        ESP_LOGW(TAG, "[BOARD %02X] Initial display flush failed — continuing headless", get_board_tag());
        s_display_absent = true;
    }

    return CEEPEW_OK;
}

CeePewErr_t hal_ui_clear(void)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    if (s_display_absent) { return CEEPEW_OK; }
    (void)memset(s_fb, 0, ceepew_oled_get_buffer_size(s_oled));
    for (uint8_t tc = 0U; tc < HAL_UI_NUM_TILE_COLS; tc++) {
        s_tile_dirty[tc] = 0xFFU;
    }
    s_framebuffer_dirty = true;
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_flush(void)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    if (s_display_absent) { return CEEPEW_OK; }

    /* Skip the I2C push entirely if nothing changed since the last
     * flush. Static screens (info, error, ...) hit this path
     * hit this path every tick after the first draw, leaving the bus
     * free for BLE. */
    if (!s_framebuffer_dirty) {
        return CEEPEW_OK;
    }

    /* Tile-dirty threshold: count the number of dirty (col,row) tiles
     * and pick the faster path. Each tile is one page-byte slice
     * (8 px tall x 8 px wide = 64 pixels). 1 tile ~4.8 ms (16 I2C
     * transactions per page-byte area), full frame ~20 ms (2
     * transactions for the whole 1024-byte framebuffer). 8 tiles is
     * roughly the break-even point; above that, the full frame push
     * is faster because it uses the panel's horizontal-addressing
     * mode. */
    uint8_t dirty_tiles = 0U;
    for (uint8_t tc = 0U; tc < HAL_UI_NUM_TILE_COLS; tc++) {
        uint8_t rows = s_tile_dirty[tc];
        while (rows != 0U) {
            dirty_tiles++;
            rows = (uint8_t)(rows & (uint8_t)(rows - 1U));
        }
    }

    /* Diagnostic: count non-zero bytes (only every 5s or first time). */
    const size_t buf_size = ceepew_oled_get_buffer_size(s_oled);
    uint32_t nonzero_bytes = 0U;
    uint8_t  first_nonzero_idx = 0U;
    bool first_set = false;
    for (size_t i = 0U; i < buf_size; i++) {
        if (s_fb[i] != 0U) {
            nonzero_bytes++;
            if (!first_set) {
                first_nonzero_idx = (uint8_t)(i & 0xFFU);
                first_set = true;
            }
        }
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    if ((!s_nonzero_flush_seen && (nonzero_bytes > 0U)) ||
        ((now_ms - s_last_flush_diag_ms) > 5000U)) {
        ESP_LOGI(TAG, "flush diag: nonzero=%lu first_idx=0x%02X tiles=%u",
                 (unsigned long)nonzero_bytes,
                 (unsigned int)first_nonzero_idx,
                 (unsigned)dirty_tiles);
        s_last_flush_diag_ms = now_ms;
        if (nonzero_bytes > 0U) { s_nonzero_flush_seen = true; }
    }

    if (dirty_tiles > 0U && dirty_tiles <= 8U) {
        const CeePewErr_t err = ssd1306_display_push_tiled();
        if (err == CEEPEW_OK) {
            return CEEPEW_OK;
        }
        /* Tile push failed: fall through to the full-frame path as a
         * last-ditch attempt. */
        ESP_LOGW(TAG, "tiled push failed, falling back to full frame");
    }
    return ssd1306_display_push();
}

/* ── Framebuffer write helper ─────────────────────────────────────── */

static inline void fb_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= HAL_UI_WIDTH_PX || y >= HAL_UI_HEIGHT_PX) { return; }
    /* Standard SSD1306 page layout: 8 pages x 128 columns, LSB-first
     * vertical. The ceepew_oled framebuffer is the same layout, so
     * the math is identical to the previous vendored-driver path. */
    const uint8_t  page = (uint8_t)(y >> 3);
    const uint8_t  bit  = (uint8_t)(1U << (y & 0x07U));
    uint8_t       *byte = &s_fb[(uint16_t)page * 128U + (uint16_t)x];
    if (on) { *byte |= bit; }
    else    { *byte = (uint8_t)(*byte & ~bit); }
    hal_ui_mark_tile_dirty(x, y);
}

CeePewErr_t hal_ui_pixel(uint8_t x, uint8_t y, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    if (s_display_absent) { return CEEPEW_OK; }
    CEEPEW_ASSERT(x < HAL_UI_WIDTH_PX && y < HAL_UI_HEIGHT_PX, CEEPEW_ERR_BOUNDS);

    switch (color) {
        case HAL_UI_WHITE:  fb_pixel(x, y, true);  break;
        case HAL_UI_BLACK:  fb_pixel(x, y, false); break;
        case HAL_UI_INVERT: {
            const uint8_t page = (uint8_t)(y >> 3);
            const uint8_t bit  = (uint8_t)(1U << (y & 0x07U));
            uint8_t *byte = &s_fb[(uint16_t)page * 128U + (uint16_t)x];
            *byte = (uint8_t)(*byte ^ bit);
            hal_ui_mark_tile_dirty(x, y);
            break;
        }
        default: return CEEPEW_ERR_PARAM;
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_hline(uint8_t x0, uint8_t x1, uint8_t y, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    if (s_display_absent) { return CEEPEW_OK; }
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
    if (s_display_absent) { return CEEPEW_OK; }
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
    if (s_display_absent) { return CEEPEW_OK; }
    CeePewErr_t err = ceepew_oled_gfx_line(s_oled, x0, y0, x1, y1, color);
    if (err == CEEPEW_OK) {
        uint8_t min_x = (x0 < x1) ? x0 : x1;
        uint8_t max_x = (x0 > x1) ? x0 : x1;
        uint8_t min_y = (y0 < y1) ? y0 : y1;
        uint8_t max_y = (y0 > y1) ? y0 : y1;
        hal_ui_mark_tiles_in_rect(min_x, min_y, (uint8_t)(max_x - min_x + 1U), (uint8_t)(max_y - min_y + 1U));
    }
    return err;
}

CeePewErr_t hal_ui_rect(const HalUIRect_t *r, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    if (s_display_absent) { return CEEPEW_OK; }
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CeePewErr_t err = ceepew_oled_gfx_rect(s_oled, r, color);
    if (err == CEEPEW_OK) {
        hal_ui_mark_tiles_in_rect(r->x, r->y, r->w, r->h);
    }
    return err;
}

CeePewErr_t hal_ui_rect_fill(const HalUIRect_t *r, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    if (s_display_absent) { return CEEPEW_OK; }
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CeePewErr_t err = ceepew_oled_gfx_rect_fill(s_oled, r, color);
    if (err == CEEPEW_OK) {
        hal_ui_mark_tiles_in_rect(r->x, r->y, r->w, r->h);
    }
    return err;
}

CeePewErr_t hal_ui_circle(uint8_t cx, uint8_t cy, uint8_t radius, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    if (s_display_absent) { return CEEPEW_OK; }
    CeePewErr_t err = ceepew_oled_gfx_circle(s_oled, cx, cy, radius, color);
    if (err == CEEPEW_OK) {
        uint8_t r = radius;
        uint8_t min_x = (uint8_t)((cx >= r) ? (uint8_t)(cx - r) : 0U);
        uint8_t min_y = (uint8_t)((cy >= r) ? (uint8_t)(cy - r) : 0U);
        hal_ui_mark_tiles_in_rect(min_x, min_y, (uint8_t)(uint8_t)(cx + r) - min_x + 1U, (uint8_t)(uint8_t)(cy + r) - min_y + 1U);
    }
    return err;
}

CeePewErr_t hal_ui_circle_fill(uint8_t cx, uint8_t cy, uint8_t radius, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    if (s_display_absent) { return CEEPEW_OK; }
    CeePewErr_t err = ceepew_oled_gfx_circle_fill(s_oled, cx, cy, radius, color);
    if (err == CEEPEW_OK) {
        uint8_t r = radius;
        uint8_t min_x = (uint8_t)((cx >= r) ? (uint8_t)(cx - r) : 0U);
        uint8_t min_y = (uint8_t)((cy >= r) ? (uint8_t)(cy - r) : 0U);
        hal_ui_mark_tiles_in_rect(min_x, min_y, (uint8_t)(uint8_t)(cx + r) - min_x + 1U, (uint8_t)(uint8_t)(cy + r) - min_y + 1U);
    }
    return err;
}

CeePewErr_t hal_ui_text(uint8_t x, uint8_t y, const char *str, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    if (s_display_absent) { return CEEPEW_OK; }
    CEEPEW_ASSERT(str != NULL, CEEPEW_ERR_NULL_PTR);
    CeePewErr_t err = ceepew_oled_gfx_text(s_oled, x, y, str, color);
    if (err == CEEPEW_OK) {
        uint16_t tw = hal_ui_text_width(str);
        hal_ui_mark_tiles_in_rect(x, y, (uint8_t)((tw < 128U) ? tw : 128U), 8U);
    }
    return err;
}

CeePewErr_t hal_ui_char(uint8_t x, uint8_t y, char c, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_BUSY);
    if (s_display_absent) { return CEEPEW_OK; }

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
    /* The GFXfont adapter's first=0x20, last=0x7E, yAdvance=8 — 5 px
     * glyph + 1 px column gap = 6 px per char, matching the legacy
     * monospace assumption in ui_manager.c. Replicate the loop here
     * so hal_ui_text_width stays a leaf with no ceepew_oled_*
     * dependency. */
    uint16_t width = 0U;
    for (uint16_t i = 0U; str[i] != '\0' && i < 128U; i++) {
        width = (uint16_t)(width + 6U);
    }
    return width;
}

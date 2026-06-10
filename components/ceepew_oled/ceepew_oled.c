/* components/ceepew_oled/ceepew_oled.c
 *
 * CEE-PEW SSD1306/SH1106 OLED transport layer implementation.
 *
 * Owns:
 *  - The framebuffer (1024 bytes, in struct storage).
 *  - The protocol logic for SSD1306 vs SH1106.
 *  - The I2C bus bring-up, multi-attempt, and last-ditch scan paths.
 *
 * Does NOT own:
 *  - The i2c_master_bus_handle_t or i2c_master_dev_handle_t. The
 *    caller is responsible for i2c_del_master_bus() and
 *    i2c_master_bus_rm_device() lifecycle. (Today, the only caller
 *    is hal_ui.c, which keeps the bus alive for the entire session.)
 *
 * License: GPL-3.0-only. See /LICENSE.
 */

#include "ceepew_oled.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"

/* ── Control-byte / command-set constants ─────────────────────────── */

/* Control byte that prefixes an I2C transfer to the SSD1306:
 *   bit 7 = 0   -> Continuation: a stream of bytes follows
 *   bit 6 = 0   -> D/C#: the following bytes are command bytes
 *   bits 5..0 = 0
 * The panel expects 0x00 before a command stream and 0x40 before a
 * data stream. */
#define CEEPEW_OLED_CTRL_CMD_STREAM   0x00U
#define CEEPEW_OLED_CTRL_DATA_STREAM  0x40U

/* SSD1306 command set (subset used by the init stream). */
#define CEEPEW_OLED_CMD_DISPLAY_OFF            0xAEU
#define CEEPEW_OLED_CMD_DISPLAY_ON             0xAFU
#define CEEPEW_OLED_CMD_DISPLAY_RAM            0xA4U
#define CEEPEW_OLED_CMD_DISPLAY_NORMAL         0xA6U
#define CEEPEW_OLED_CMD_SET_MUX_RATIO          0xA8U
#define CEEPEW_OLED_CMD_SET_DISPLAY_OFFSET     0xD3U
#define CEEPEW_OLED_CMD_SET_DISPLAY_START_LINE 0x40U
#define CEEPEW_OLED_CMD_SET_SEGMENT_REMAP_1    0xA1U
#define CEEPEW_OLED_CMD_SET_COM_SCAN_MODE      0xC8U
#define CEEPEW_OLED_CMD_SET_DISPLAY_CLK_DIV    0xD5U
#define CEEPEW_OLED_CMD_SET_COM_PIN_MAP        0xDAU
#define CEEPEW_OLED_CMD_SET_CONTRAST           0x81U
#define CEEPEW_OLED_CMD_SET_VCOMH_DESELCT      0xDBU
#define CEEPEW_OLED_CMD_SET_MEMORY_ADDR_MODE   0x20U
#define CEEPEW_OLED_CMD_SET_HORI_ADDR_MODE     0x00U
#define CEEPEW_OLED_CMD_SET_PAGE_ADDR_MODE     0x02U
#define CEEPEW_OLED_CMD_SET_COLUMN_RANGE       0x21U
#define CEEPEW_OLED_CMD_SET_PAGE_RANGE         0x22U
#define CEEPEW_OLED_CMD_SET_CHARGE_PUMP        0x8DU
#define CEEPEW_OLED_CMD_DEACTIVE_SCROLL        0x2EU
#define CEEPEW_OLED_CMD_SET_PAGE_START         0xB0U
#define CEEPEW_OLED_CMD_SET_LOWER_COL          0x00U
#define CEEPEW_OLED_CMD_SET_HIGHER_COL         0x10U

/* ── Internal types ──────────────────────────────────────────────── */

#define CEEPEW_OLED_FAST_HZ    800000U   /* Fm+ fallback speed        */
#define CEEPEW_OLED_SLOW_HZ    400000U   /* Default operational speed */
#define CEEPEW_OLED_TILE_COLS  16U       /* 128 cols / 8 px per tile  */
#define CEEPEW_OLED_TILE_ROWS  8U        /* 64 rows / 8 px per tile   */

struct ceepew_oled_t {
    i2c_master_bus_handle_t  bus;          /* may be NULL if 800 kHz
                                              fallback disabled        */
    i2c_master_dev_handle_t  i2c_dev;      /* primary dev, slow speed  */
    i2c_master_dev_handle_t  i2c_dev_fast; /* optional 800 kHz dev     */
    uint8_t                  addr;
    uint8_t                  buffer[CEEPEW_OLED_BUF_SIZE];
    bool                     sh1106_mode;
    bool                     initialised;
    bool                     fast_probed;  /* 800 kHz probe ACKed      */
    bool                     fast_active;  /* currently using 800 kHz  */
    bool                     fast_failed;  /* 800 kHz already tried
                                              and failed this session  */
};

/* ── Logging tag ─────────────────────────────────────────────────── */

static const char *TAG = "ceepew_oled";

/* ── Lifecycle ──────────────────────────────────────────────────── */

ceepew_oled_t *ceepew_oled_create(void)
{
    assert(sizeof(ceepew_oled_t) > 0U);

    /* Region allocator is the project's only allowed dynamic source;
     * for the OLED handle we use static storage so the struct outlives
     * any region_free() calls during session teardown. */
    static ceepew_oled_t s_dev;
    (void)memset(&s_dev, 0, sizeof(s_dev));
    s_dev.bus         = NULL;
    s_dev.i2c_dev     = NULL;
    s_dev.i2c_dev_fast = NULL;
    s_dev.addr        = 0U;
    s_dev.sh1106_mode = false;
    s_dev.initialised = false;
    s_dev.fast_probed = false;
    s_dev.fast_active = false;
    s_dev.fast_failed = false;
    return &s_dev;
}

void ceepew_oled_destroy(ceepew_oled_t *dev)
{
    assert(dev != NULL);
    /* The I2C bus and device are caller-owned; we just zero our
     * own state. Use the secure-zero pattern so a future use-after-
     * destroy can't accidentally read stale framebuffer data. */
    volatile uint8_t *p = dev->buffer;
    for (size_t i = 0U; i < CEEPEW_OLED_BUF_SIZE; i++) {
        p[i] = 0U;
    }
    __asm__ __volatile__("" ::: "memory");
    dev->bus          = NULL;
    dev->i2c_dev      = NULL;
    dev->i2c_dev_fast = NULL;
    dev->addr         = 0U;
    dev->sh1106_mode  = false;
    dev->initialised  = false;
    dev->fast_probed  = false;
    dev->fast_active  = false;
    dev->fast_failed  = false;
}

/* ── Framebuffer access ──────────────────────────────────────────── */

uint8_t *ceepew_oled_get_buffer(ceepew_oled_t *dev)
{
    assert(dev != NULL);
    return dev->buffer;
}

size_t ceepew_oled_get_buffer_size(const ceepew_oled_t *dev)
{
    assert(dev != NULL);
    return CEEPEW_OLED_BUF_SIZE;
}

void ceepew_oled_clear_buffer(ceepew_oled_t *dev)
{
    assert(dev != NULL);
    (void)memset(dev->buffer, 0, CEEPEW_OLED_BUF_SIZE);
}

bool ceepew_oled_get_sh1106_mode(const ceepew_oled_t *dev)
{
    assert(dev != NULL);
    return dev->sh1106_mode;
}

/* ── SSD1306 init command stream ─────────────────────────────────── */

/* Issue the standard SSD1306 128x64 init command stream. Sequence
 * mirrors the SSD1306 datasheet section "Software Initialization" and
 * the values used by the prior CEE-PEW vendored driver. The init
 * stream fits in a 32-byte stack buffer. */
static esp_err_t send_init_stream(ceepew_oled_t *dev)
{
    assert(dev != NULL);
    assert(dev->i2c_dev != NULL);

    uint8_t out[32U];
    uint8_t i = 0U;
    out[i++] = CEEPEW_OLED_CTRL_CMD_STREAM;
    out[i++] = CEEPEW_OLED_CMD_DISPLAY_OFF;             /* AE          */
    out[i++] = CEEPEW_OLED_CMD_SET_MUX_RATIO;          /* A8          */
    out[i++] = 0x3FU;                                   /* 64 MUX      */
    out[i++] = CEEPEW_OLED_CMD_SET_DISPLAY_OFFSET;     /* D3          */
    out[i++] = 0x00U;
    out[i++] = CEEPEW_OLED_CMD_SET_DISPLAY_START_LINE; /* 40          */
    out[i++] = CEEPEW_OLED_CMD_SET_SEGMENT_REMAP_1;    /* A1          */
    out[i++] = CEEPEW_OLED_CMD_SET_COM_SCAN_MODE;      /* C8          */
    out[i++] = CEEPEW_OLED_CMD_SET_DISPLAY_CLK_DIV;    /* D5          */
    out[i++] = 0x80U;
    out[i++] = CEEPEW_OLED_CMD_SET_COM_PIN_MAP;        /* DA          */
    out[i++] = 0x12U;                                   /* alt COM cfg */
    out[i++] = CEEPEW_OLED_CMD_SET_CONTRAST;           /* 81          */
    out[i++] = 0xFFU;
    out[i++] = CEEPEW_OLED_CMD_DISPLAY_RAM;            /* A4          */
    out[i++] = CEEPEW_OLED_CMD_SET_VCOMH_DESELCT;      /* DB          */
    out[i++] = 0x40U;
    out[i++] = CEEPEW_OLED_CMD_SET_MEMORY_ADDR_MODE;   /* 20          */
    out[i++] = CEEPEW_OLED_CMD_SET_PAGE_ADDR_MODE;     /* 02 (page)   */
    out[i++] = CEEPEW_OLED_CMD_SET_LOWER_COL;          /* 00          */
    out[i++] = CEEPEW_OLED_CMD_SET_HIGHER_COL;         /* 10          */
    out[i++] = CEEPEW_OLED_CMD_SET_CHARGE_PUMP;        /* 8D          */
    out[i++] = 0x14U;                                   /* pump on     */
    out[i++] = CEEPEW_OLED_CMD_DEACTIVE_SCROLL;        /* 2E          */
    out[i++] = CEEPEW_OLED_CMD_DISPLAY_NORMAL;         /* A6          */
    out[i++] = CEEPEW_OLED_CMD_DISPLAY_ON;             /* AF          */

    const uint8_t expected = 28U;
    assert((i) < (expected));

    esp_err_t rc = i2c_master_transmit(dev->i2c_dev, out, i,
                                       CEEPEW_OLED_I2C_TIMEOUT_TICKS);
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "OLED panel configured for 128x64");
    } else {
        ESP_LOGE(TAG, "init stream failed: %d (%s)",
                 (int)rc, esp_err_to_name(rc));
    }
    return rc;
}

/* Probe whether the panel ACKs at 800 kHz (Fm+). If it does, store
 * a second i2c_master_dev_handle_t in dev->i2c_dev_fast so the
 * display() fast path can use it on first failure at 400 kHz.
 *
 * Never blocks boot. Worst case: 100 ms timeout on a panel that
 * doesn't support Fm+. The slow path stays at 400 kHz. */
static void probe_fast_mode(ceepew_oled_t *dev)
{
    if (dev->bus == NULL) {
        return;
    }
    if (dev->fast_probed || dev->fast_failed) {
        return;
    }

    const i2c_device_config_t fast_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = dev->addr,
        .scl_speed_hz    = CEEPEW_OLED_FAST_HZ,
    };
    i2c_master_dev_handle_t fast_dev = NULL;
    esp_err_t rc = i2c_master_bus_add_device(dev->bus, &fast_cfg, &fast_dev);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "Could not add 800 kHz device (rc=%d); staying at 400 kHz",
                 (int)rc);
        dev->fast_failed = true;
        return;
    }
    rc = i2c_master_probe(dev->bus, dev->addr,
                          pdMS_TO_TICKS(100U));
    if (rc == ESP_OK) {
        dev->i2c_dev_fast = fast_dev;
        dev->fast_probed  = true;
        ESP_LOGI(TAG, "Panel ACKed at 800 kHz Fm+; fast mode available");
    } else {
        ESP_LOGI(TAG, "Panel did not ACK at 800 kHz Fm+; staying at 400 kHz");
        (void)i2c_master_bus_rm_device(fast_dev);
        dev->i2c_dev_fast = NULL;
        dev->fast_failed = true;
    }
}

esp_err_t ceepew_oled_init_panel(ceepew_oled_t *dev,
                                 i2c_master_bus_handle_t bus,
                                 i2c_master_dev_handle_t i2c_dev,
                                 uint8_t addr)
{
    assert(dev != NULL);
    assert(i2c_dev != NULL);

    dev->bus          = bus;
    dev->i2c_dev      = i2c_dev;
    dev->i2c_dev_fast = NULL;
    dev->addr         = addr;
    dev->sh1106_mode  = false;
    dev->fast_probed  = false;
    dev->fast_active  = false;
    dev->fast_failed  = false;
    (void)memset(dev->buffer, 0, CEEPEW_OLED_BUF_SIZE);

    esp_err_t rc = send_init_stream(dev);
    if (rc == ESP_OK) {
        dev->initialised = true;
        /* Best-effort: probe 800 kHz support once the panel is up.
         * Never blocks boot. */
        probe_fast_mode(dev);
    }
    return rc;
}

/* ── Display push: SSD1306 fast path ─────────────────────────────── */

/* Internal: do the 2-transaction horizontal-addressing push on the
 * given device handle. Returns ESP_OK or the first I2C error. */
static esp_err_t push_full_frame(ceepew_oled_t *dev,
                                 i2c_master_dev_handle_t dev_handle)
{
    /* 1. Command stream: set horizontal addressing, then column range
     *    [0..127] and page range [0..7]. The SET_PAGE_START (0xB0) at
     *    the start pins the panel's page counter to 0 and resets the
     *    column pointer to 0; without this, a previous mid-write NACK
     *    could leave the page counter at a non-zero value, which
     *    produces the "headings pushed to the bottom" symptom. Cost:
     *    2 bytes per frame. */
    const uint8_t cmd_stream[12U] = {
        CEEPEW_OLED_CTRL_CMD_STREAM,
        CEEPEW_OLED_CMD_SET_PAGE_START,                    /* page 0      */
        CEEPEW_OLED_CMD_SET_LOWER_COL,                     /* col low  0  */
        CEEPEW_OLED_CMD_SET_HIGHER_COL,                    /* col high 0  */
        CEEPEW_OLED_CMD_SET_MEMORY_ADDR_MODE,
        CEEPEW_OLED_CMD_SET_HORI_ADDR_MODE,                /* horizontal  */
        CEEPEW_OLED_CMD_SET_COLUMN_RANGE, 0x00U, 0x7FU,     /* col [0..127]*/
        CEEPEW_OLED_CMD_SET_PAGE_RANGE,   0x00U, 0x07U,     /* page [0..7] */
    };

    esp_err_t rc = i2c_master_transmit(dev_handle, cmd_stream,
                                       sizeof(cmd_stream),
                                       CEEPEW_OLED_I2C_TIMEOUT_TICKS);
    if (rc != ESP_OK) {
        return rc;
    }

    /* 2. Data transaction: 0x40 control byte + 1024 framebuffer bytes.
     *    Static BSS so the 4 KB UI task stack is reserved for render
     *    recursion / call-graph depth, not frame transport. */
    static uint8_t s_data_stream[1U + CEEPEW_OLED_BUF_SIZE];
    s_data_stream[0U] = CEEPEW_OLED_CTRL_DATA_STREAM;
    (void)memcpy(&s_data_stream[1U], dev->buffer, CEEPEW_OLED_BUF_SIZE);

    rc = i2c_master_transmit(dev_handle, s_data_stream,
                             sizeof(s_data_stream),
                             CEEPEW_OLED_I2C_TIMEOUT_TICKS);
    return rc;
}

esp_err_t ceepew_oled_display(ceepew_oled_t *dev)
{
    assert(dev != NULL);
    assert(dev->initialised);
    assert(dev->i2c_dev != NULL);

    /* Attempt 1: primary (400 kHz) device. */
    esp_err_t rc = push_full_frame(dev, dev->i2c_dev);
    if (rc == ESP_OK) {
        if (dev->fast_active) {
            dev->fast_active = false;
            ESP_LOGI(TAG, "Display push recovered on slow path; falling back to 400 kHz");
        }
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Display push at 400 kHz failed: %d (%s)",
             (int)rc, esp_err_to_name(rc));

    /* Attempt 2: 800 kHz Fm+ fallback. Only attempted once per session —
     * if the 800 kHz path also fails, we log and stay on the slow
     * device so the rest of the UI loop is not blocked. */
    if (dev->fast_probed && !dev->fast_failed && dev->i2c_dev_fast != NULL) {
        ESP_LOGW(TAG, "Retrying display push at 800 kHz Fm+");
        esp_err_t fast_rc = push_full_frame(dev, dev->i2c_dev_fast);
        if (fast_rc == ESP_OK) {
            dev->fast_active = true;
            ESP_LOGI(TAG, "Display push succeeded at 800 kHz Fm+; fast mode engaged");
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Display push at 800 kHz Fm+ failed: %d (%s); "
                      "panel does not support Fm+; staying at 400 kHz",
                 (int)fast_rc, esp_err_to_name(fast_rc));
        dev->fast_failed = true;
    } else {
        ESP_LOGE(TAG, "800 kHz fallback not available; staying at 400 kHz");
    }
    return rc;
}

/* ── Display push: SH1106 slow path (per-page, +col_offset) ──────── */

esp_err_t ceepew_oled_display_sh1106(ceepew_oled_t *dev, uint8_t col_offset)
{
    assert(dev != NULL);
    assert(dev->initialised);
    assert(dev->i2c_dev != NULL);

    /* SH1106 does not support SET_COLUMN_RANGE / SET_PAGE_RANGE in
     * horizontal mode, so we fall back to the per-page page-addressing
     * path. col_offset is typically 2 for the common SH1106 panels
     * (0x3C and 0x3D both). 8 pages x 2 I2C transactions = 16
     * transactions per frame. */
    for (uint8_t page = 0U; page < CEEPEW_OLED_PAGES; page++) {
        const uint8_t col_low  = (uint8_t)(col_offset & 0x0FU);
        const uint8_t col_high = (uint8_t)(0x10U | ((col_offset >> 4U) & 0x0FU));
        const uint8_t page_cmd = (uint8_t)(CEEPEW_OLED_CMD_SET_PAGE_START | page);

        const uint8_t cmd_stream[4U] = {
            CEEPEW_OLED_CTRL_CMD_STREAM,
            col_low,
            col_high,
            page_cmd,
        };
        esp_err_t rc = i2c_master_transmit(dev->i2c_dev, cmd_stream,
                                           sizeof(cmd_stream),
                                           CEEPEW_OLED_I2C_TIMEOUT_TICKS);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "page %u cmd failed: %d (%s)",
                     (unsigned)page, (int)rc, esp_err_to_name(rc));
            return rc;
        }

        uint8_t data_stream[1U + CEEPEW_OLED_WIDTH_PX];
        data_stream[0U] = CEEPEW_OLED_CTRL_DATA_STREAM;
        (void)memcpy(&data_stream[1U],
                     &dev->buffer[(uint16_t)page * CEEPEW_OLED_WIDTH_PX],
                     CEEPEW_OLED_WIDTH_PX);

        rc = i2c_master_transmit(dev->i2c_dev, data_stream,
                                 sizeof(data_stream),
                                 CEEPEW_OLED_I2C_TIMEOUT_TICKS);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "page %u data failed: %d (%s)",
                     (unsigned)page, (int)rc, esp_err_to_name(rc));
            return rc;
        }
    }

    dev->sh1106_mode = true;
    return ESP_OK;
}

/* ── Display push: tile (8 col x 8 row block) ─────────────────────── */

esp_err_t ceepew_oled_push_tile(ceepew_oled_t *dev,
                                uint8_t tile_col, uint8_t tile_row)
{
    assert(dev != NULL);
    assert(dev->initialised);
    assert(dev->i2c_dev != NULL);
    assert(tile_col < CEEPEW_OLED_TILE_COLS);
    assert(tile_row < CEEPEW_OLED_TILE_ROWS);

    /* For a tile (col_tile, row_tile) spanning columns [c*8..c*8+7]
     * and rows [r*8..r*8+7], the framebuffer bytes are scattered:
     * page p (0..7) holds the 8 rows of the tile at columns c*8..c*8+7,
     * so byte index = p * 128 + c*8 + 0..7. We push 8 pages in 16
     * I2C transactions (one command, one data, per page).
     *
     * Note: this is slower per byte than the 2-transaction full-frame
     * push, so it only pays off when <= 8 tiles are dirty. Above that
     * threshold hal_ui_flush falls back to ceepew_oled_display. */
    const uint8_t col_start = (uint8_t)(tile_col * 8U);
    const uint8_t col_low   = (uint8_t)(col_start & 0x0FU);
    const uint8_t col_high  = (uint8_t)(0x10U | ((col_start >> 4U) & 0x0FU));
    i2c_master_dev_handle_t dev_handle = dev->fast_active ? dev->i2c_dev_fast
                                                           : dev->i2c_dev;
    if (dev_handle == NULL) { dev_handle = dev->i2c_dev; }

    for (uint8_t page = 0U; page < CEEPEW_OLED_PAGES; page++) {
        const uint8_t page_cmd = (uint8_t)(CEEPEW_OLED_CMD_SET_PAGE_START | page);
        const uint8_t cmd[4U] = {
            CEEPEW_OLED_CTRL_CMD_STREAM,
            col_low, col_high, page_cmd,
        };
        esp_err_t rc = i2c_master_transmit(dev_handle, cmd,
                                           sizeof(cmd),
                                           CEEPEW_OLED_I2C_TIMEOUT_TICKS);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "tile (%u,%u) page %u cmd failed: %d (%s)",
                     (unsigned)tile_col, (unsigned)tile_row,
                     (unsigned)page, (int)rc, esp_err_to_name(rc));
            return rc;
        }
        uint8_t data[1U + 8U];
        data[0U] = CEEPEW_OLED_CTRL_DATA_STREAM;
        const uint16_t row_start = (uint16_t)page * (uint16_t)CEEPEW_OLED_WIDTH_PX
                                  + (uint16_t)col_start;
        (void)memcpy(&data[1U], &dev->buffer[row_start], 8U);
        rc = i2c_master_transmit(dev_handle, data, sizeof(data),
                                 CEEPEW_OLED_I2C_TIMEOUT_TICKS);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "tile (%u,%u) page %u data failed: %d (%s)",
                     (unsigned)tile_col, (unsigned)tile_row,
                     (unsigned)page, (int)rc, esp_err_to_name(rc));
            return rc;
        }
    }
    return ESP_OK;
}

/* ── Boot bring-up helpers ───────────────────────────────────────── */

esp_err_t ceepew_oled_probe_with_retry(i2c_master_bus_handle_t bus, uint8_t addr)
{
    assert(bus != NULL);
    assert(addr >= 0x03U && addr <= 0x77U);

    for (uint8_t attempt = 0U; attempt < CEEPEW_OLED_PROBE_ATTEMPTS; attempt++) {
        esp_err_t rc = i2c_master_probe(bus, addr,
                                        CEEPEW_OLED_I2C_TIMEOUT_TICKS);
        if (rc == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGD(TAG, "probe 0x%02X attempt %u/%u failed (%d)",
                 (unsigned)addr,
                 (unsigned)(attempt + 1U),
                 (unsigned)CEEPEW_OLED_PROBE_ATTEMPTS,
                 (int)rc);
        if ((attempt + 1U) < CEEPEW_OLED_PROBE_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(CEEPEW_OLED_PROBE_RETRY_MS));
        }
    }
    return ESP_FAIL;
}

void ceepew_oled_bus_recover(gpio_num_t sda_pin, gpio_num_t scl_pin)
{
    assert(GPIO_IS_VALID_GPIO(sda_pin) &&
                  GPIO_IS_VALID_GPIO(scl_pin));

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
        if (gpio_get_level(sda_pin) != 0) {
            break;
        }
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
    /* Release pins back to default state so the I2C driver can claim them */
    gpio_reset_pin(sda_pin);
    gpio_reset_pin(scl_pin);
    vTaskDelay(pdMS_TO_TICKS(10U));
}

esp_err_t ceepew_oled_bus_bringup(gpio_num_t sda, gpio_num_t scl,
                                  uint32_t speed_hz, uint8_t addr,
                                  i2c_master_bus_handle_t *out_bus,
                                  i2c_master_dev_handle_t *out_dev)
{
    assert(out_bus != NULL && out_dev != NULL);
    assert(sda != scl);
    *out_bus = NULL;
    *out_dev = NULL;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port              = I2C_NUM_0,
        .sda_io_num            = sda,
        .scl_io_num            = scl,
        .clk_source            = I2C_CLK_SRC_APB,
        .glitch_ignore_cnt     = 0U,
        .intr_priority         = 0,
        /* Synchronous mode: no async queue, every i2c_master_transmit()
         * blocks until the physical transfer completes. Prevents the
         * async queue overflow that caused hangs in the prior driver. */
        .trans_queue_depth     = 1U,
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

    if (ceepew_oled_probe_with_retry(bus, addr) != ESP_OK) {
        ESP_LOGW(TAG, "No ACK from 0x%02X on SDA=%d SCL=%d @%luHz after %u attempts",
                 (unsigned)addr, (int)sda, (int)scl,
                 (unsigned long)speed_hz,
                 (unsigned)CEEPEW_OLED_PROBE_ATTEMPTS);
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

esp_err_t ceepew_oled_multi_attempt(const ceepew_oled_pins_t *pins,
                                    i2c_master_bus_handle_t *out_bus,
                                    i2c_master_dev_handle_t *out_dev,
                                    uint8_t *out_addr)
{
    assert(pins != NULL);
    assert(out_bus != NULL);
    assert(out_dev != NULL && out_addr != NULL);

    *out_bus  = NULL;
    *out_dev  = NULL;
    *out_addr = 0U;

    const uint32_t speeds[2] = { pins->speed_primary_hz,
                                 pins->speed_fallback_hz };
    const uint8_t  addrs[2]  = { pins->addr_primary,
                                 pins->addr_fallback };
    const gpio_num_t sda_pins[2] = { pins->sda_primary,
                                     pins->sda_fallback };
    const gpio_num_t scl_pins[2] = { pins->scl_primary,
                                     pins->scl_fallback };

    for (uint8_t pi = 0U; pi < 2U; pi++) {
        if (pi == 1U &&
            sda_pins[1] == sda_pins[0] &&
            scl_pins[1] == scl_pins[0]) {
            continue;
        }
        if (!GPIO_IS_VALID_GPIO(sda_pins[pi]) ||
            !GPIO_IS_VALID_GPIO(scl_pins[pi])) {
            continue;
        }

        ESP_LOGI(TAG, "Trying I2C pins SDA=%d SCL=%d",
                 (int)sda_pins[pi], (int)scl_pins[pi]);
        ceepew_oled_bus_recover(sda_pins[pi], scl_pins[pi]);

        for (uint8_t si = 0U; si < 2U; si++) {
            if (si == 1U && speeds[1] == speeds[0]) {
                continue;
            }
            for (uint8_t ai = 0U; ai < 2U; ai++) {
                i2c_master_bus_handle_t bus = NULL;
                i2c_master_dev_handle_t dev = NULL;
                esp_err_t rc = ceepew_oled_bus_bringup(sda_pins[pi],
                                                       scl_pins[pi],
                                                       speeds[si],
                                                       addrs[ai],
                                                       &bus, &dev);
                if (rc != ESP_OK) {
                    continue;
                }
                ESP_LOGI(TAG, "OLED ready at 0x%02X (SDA=%d SCL=%d @%luHz)",
                         (unsigned)addrs[ai],
                         (int)sda_pins[pi], (int)scl_pins[pi],
                         (unsigned long)speeds[si]);
                *out_bus  = bus;
                *out_dev  = dev;
                *out_addr = addrs[ai];
                /* Caller will eventually call i2c_del_master_bus(bus)
                 * when the panel is deinitialised. We deliberately do
                 * not delete the bus here so the panel stays usable
                 * for the lifetime of the application. */
                return ESP_OK;
            }
        }
    }

    return ESP_FAIL;
}

esp_err_t ceepew_oled_scan_all_pins(i2c_master_bus_handle_t *out_bus,
                                    i2c_master_dev_handle_t *out_dev,
                                    uint8_t *out_addr)
{
    assert(out_bus != NULL);
    assert(out_dev != NULL && out_addr != NULL);

    *out_bus  = NULL;
    *out_dev  = NULL;
    *out_addr = 0U;

    static const gpio_num_t all_pins[] = {
        GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_12, GPIO_NUM_13,
        GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17,
        GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_21, GPIO_NUM_22,
        GPIO_NUM_23, GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_27,
        GPIO_NUM_32, GPIO_NUM_33
    };
    static const uint8_t addrs[2] = { 0x3CU, 0x3DU };
    static const uint8_t num_pins = (uint8_t)(sizeof(all_pins) /
                                              sizeof(all_pins[0]));
    assert(num_pins > 0U);
    assert(num_pins <= 32U);

    for (uint8_t sda_idx = 0U; sda_idx < num_pins; sda_idx++) {
        for (uint8_t scl_idx = 0U; scl_idx < num_pins; scl_idx++) {
            if (sda_idx == scl_idx ||
                !GPIO_IS_VALID_GPIO(all_pins[sda_idx]) ||
                !GPIO_IS_VALID_GPIO(all_pins[scl_idx])) {
                continue;
            }
            ceepew_oled_bus_recover(all_pins[sda_idx], all_pins[scl_idx]);
            for (uint8_t ai = 0U; ai < 2U; ai++) {
                i2c_master_bus_handle_t bus = NULL;
                i2c_master_dev_handle_t dev = NULL;
                esp_err_t rc = ceepew_oled_bus_bringup(all_pins[sda_idx],
                                                       all_pins[scl_idx],
                                                       400000U,
                                                       addrs[ai],
                                                       &bus, &dev);
                if (rc != ESP_OK) {
                    continue;
                }
                ESP_LOGW(TAG, "*** FOUND OLED at 0x%02X on SDA=%d SCL=%d ***",
                         (unsigned)addrs[ai],
                         (int)all_pins[sda_idx],
                         (int)all_pins[scl_idx]);
                *out_bus  = bus;
                *out_dev  = dev;
                *out_addr = addrs[ai];
                return ESP_OK;
            }
        }
    }
    return ESP_FAIL;
}

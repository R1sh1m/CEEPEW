/* components/ceepew_oled/ceepew_oled.c
 *
 * CEE-PEW SSD1306/SH1106 OLED transport layer implementation.
 *
 * Owns:
 *  - The framebuffer (1024 bytes, in struct storage).
 *  - The protocol logic for SSD1306 vs SH1106.
 *  - The I2C bus init (nopnop2002 pattern: create bus → add device →
 *    send init stream; no probe, no bus recovery, no scan).
 *
 * Does NOT own:
 *  - The i2c_master_bus_handle_t or i2c_master_dev_handle_t lifetime.
 *    The caller creates the bus via ceepew_oled_bus_init() and is
 *    responsible for i2c_del_master_bus() if needed.
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
#include "hal_pins.h"

/* ── Control-byte / command-set constants ─────────────────────────── */

#define CEEPEW_OLED_CTRL_CMD_STREAM   0x00U
#define CEEPEW_OLED_CTRL_DATA_STREAM  0x40U

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

#define CEEPEW_OLED_FAST_HZ    800000U
#define CEEPEW_OLED_SLOW_HZ    400000U
#define CEEPEW_OLED_TILE_COLS  16U
#define CEEPEW_OLED_TILE_ROWS  8U

struct ceepew_oled_t {
    i2c_master_bus_handle_t  bus;
    i2c_master_dev_handle_t  i2c_dev;
    i2c_master_dev_handle_t  i2c_dev_fast;
    uint8_t                  addr;
    uint8_t                  buffer[CEEPEW_OLED_BUF_SIZE];
    bool                     sh1106_mode;
    bool                     initialised;
    bool                     fast_probed;
    bool                     fast_active;
    bool                     fast_failed;
};

/* ── Logging tag ─────────────────────────────────────────────────── */

static const char *TAG = "ceepew_oled";

/* ── Lifecycle ──────────────────────────────────────────────────── */

static ceepew_oled_t s_dev;
static ceepew_oled_t s_backup_dev;
static bool           s_has_backup = false;

void ceepew_oled_backup_state(void)
{
    s_backup_dev = s_dev;
    s_has_backup = true;
}

void ceepew_oled_restore_state(void)
{
    if (s_has_backup) {
        s_dev = s_backup_dev;
    }
}

ceepew_oled_t *ceepew_oled_create(void)
{
    assert(sizeof(ceepew_oled_t) > 0U);

    (void)memset(&s_dev, 0, sizeof(s_dev));
    s_dev.bus          = NULL;
    s_dev.i2c_dev      = NULL;
    s_dev.i2c_dev_fast = NULL;
    s_dev.addr         = 0U;
    s_dev.sh1106_mode  = false;
    s_dev.initialised  = false;
    s_dev.fast_probed  = false;
    s_dev.fast_active  = false;
    s_dev.fast_failed  = false;
    return &s_dev;
}

void ceepew_oled_destroy(ceepew_oled_t *dev)
{
    assert(dev != NULL);
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

/* ── I2C bus init (nopnop2002 pattern) ───────────────────────────── */

esp_err_t ceepew_oled_bus_init(i2c_master_bus_handle_t *out_bus,
                               i2c_master_dev_handle_t *out_dev,
                               gpio_num_t sda, gpio_num_t scl,
                               uint32_t speed_hz,
                               uint8_t addr)
{
    assert(out_bus != NULL && out_dev != NULL);
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
         * blocks until the physical transfer completes. */
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

    ESP_LOGI(TAG, "I2C bus created — SDA=%d SCL=%d, device 0x%02X added (no probe)",
             (int)sda, (int)scl, (unsigned)addr);

    *out_bus = bus;
    *out_dev = dev;
    return ESP_OK;
}

/* ── SSD1306 init command stream ─────────────────────────────────── */

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

/* ── SH1106 init command stream ─────────────────────────────────── */

static esp_err_t send_init_stream_sh1106(ceepew_oled_t *dev)
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

    esp_err_t rc = i2c_master_transmit(dev->i2c_dev, out, i,
                                       CEEPEW_OLED_I2C_TIMEOUT_TICKS);
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "SH1106 panel configured for 128x64");
    } else {
        ESP_LOGE(TAG, "SH1106 init stream failed: %d (%s)",
                 (int)rc, esp_err_to_name(rc));
    }
    return rc;
}

/* ── Fast-mode probe (opt-in 800 kHz fallback) ──────────────────── */

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

    const uint8_t dummy = 0x00;
    rc = i2c_master_transmit(fast_dev, &dummy, 1, pdMS_TO_TICKS(100U));
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

bool ceepew_oled_probe_fast_mode(i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (bus == NULL) {
        return false;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = CEEPEW_OLED_FAST_HZ,
    };
    i2c_master_dev_handle_t probe_dev = NULL;
    esp_err_t rc = i2c_master_bus_add_device(bus, &cfg, &probe_dev);
    if (rc != ESP_OK) {
        return false;
    }

    const uint8_t dummy = 0x00;
    rc = i2c_master_transmit(probe_dev, &dummy, 1, pdMS_TO_TICKS(100U));
    (void)i2c_master_bus_rm_device(probe_dev);

    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "Device 0x%02X ACKs in fast (800 kHz) mode", addr);
    }
    return (rc == ESP_OK);
}

/* ── Init panel ──────────────────────────────────────────────────── */

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
        probe_fast_mode(dev);
    }
    return rc;
}

esp_err_t ceepew_oled_init_panel_sh1106(ceepew_oled_t *dev,
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

    esp_err_t rc = send_init_stream_sh1106(dev);
    if (rc == ESP_OK) {
        dev->initialised = true;
        probe_fast_mode(dev);
    }
    return rc;
}

/* ── Display push: SSD1306 fast path ─────────────────────────────── */

static esp_err_t push_full_frame(ceepew_oled_t *dev,
                                 i2c_master_dev_handle_t dev_handle)
{
    const uint8_t cmd_stream[12U] = {
        CEEPEW_OLED_CTRL_CMD_STREAM,
        CEEPEW_OLED_CMD_SET_PAGE_START,
        CEEPEW_OLED_CMD_SET_LOWER_COL,
        CEEPEW_OLED_CMD_SET_HIGHER_COL,
        CEEPEW_OLED_CMD_SET_MEMORY_ADDR_MODE,
        CEEPEW_OLED_CMD_SET_HORI_ADDR_MODE,
        CEEPEW_OLED_CMD_SET_COLUMN_RANGE, 0x00U, 0x7FU,
        CEEPEW_OLED_CMD_SET_PAGE_RANGE,   0x00U, 0x07U,
    };

    esp_err_t rc = i2c_master_transmit(dev_handle, cmd_stream,
                                       sizeof(cmd_stream),
                                       CEEPEW_OLED_I2C_TIMEOUT_TICKS);
    if (rc != ESP_OK) {
        return rc;
    }

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

/* ── Contrast / invert ───────────────────────────────────────────── */

esp_err_t ceepew_oled_set_contrast(ceepew_oled_t *dev, uint8_t contrast)
{
    assert(dev != NULL);
    assert(dev->initialised);
    assert(dev->i2c_dev != NULL);

    const uint8_t cmd[2U] = {
        CEEPEW_OLED_CTRL_CMD_STREAM,
        CEEPEW_OLED_CMD_SET_CONTRAST,
    };
    esp_err_t rc = i2c_master_transmit(dev->i2c_dev, cmd, sizeof(cmd),
                                       CEEPEW_OLED_I2C_TIMEOUT_TICKS);
    if (rc != ESP_OK) {
        return rc;
    }
    const uint8_t data[1U] = { contrast };
    return i2c_master_transmit(dev->i2c_dev, data, sizeof(data),
                               CEEPEW_OLED_I2C_TIMEOUT_TICKS);
}

esp_err_t ceepew_oled_set_invert(ceepew_oled_t *dev, bool invert)
{
    assert(dev != NULL);
    assert(dev->initialised);
    assert(dev->i2c_dev != NULL);

    const uint8_t cmd[2U] = {
        CEEPEW_OLED_CTRL_CMD_STREAM,
        invert ? 0xA7U : 0xA6U,
    };
    return i2c_master_transmit(dev->i2c_dev, cmd, sizeof(cmd),
                               CEEPEW_OLED_I2C_TIMEOUT_TICKS);
}

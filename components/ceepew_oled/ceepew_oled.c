/* components/ceepew_oled/ceepew_oled.c
 *
 * CEE-PEW SSD1306/SH1106 OLED transport layer implementation.
 *
 * Owns:
 *  - The framebuffer (1024 bytes, in struct storage).
 *  - The protocol logic for SSD1306 vs SH1106.
 *  - The I2C bus init (Arduino Wire transport — see Test 007).
 *
 * TRANSPORT NOTE (see DEBUG_LOG_OLED_I2C.md Test 007):
 *  The ESP-IDF driver-ng API (driver/i2c_master.h) and legacy driver
 *  (driver/i2c.h) both NACK on data-phase bytes for this OLED clone.
 *  The Arduino Wire library works. This file uses the ceepew_oled_arduino
 *  component as its transport. The i2c_master_bus_handle_t /
 *  i2c_master_dev_handle_t fields in the struct are retained as non-NULL
 *  sentinels so that existing assert()s in hal_ui.c are satisfied without
 *  requiring caller changes.
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
#include "driver/i2c_master.h"  /* kept for handle typedefs used by callers */
#include "ceepew_oled_arduino_transport.h"
#include "esp_log.h"
#include "esp_err.h"
#include "../hal/hal_pins.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "ceepew_config.h"

static const char *TAG = "ceepew_oled";

bool g_oled_in_init_stream = false;

static uint8_t get_board_tag(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        return mac[5];
    }
    return 0;
}

static uint32_t s_i2c_tx_attempts = 0;

/* Sentinel handle returned to callers that check for != NULL.  The
 * Arduino Wire transport owns the actual bus; this pointer is never
 * dereferenced here. */
static uint8_t s_sentinel_bus_marker = 0xBBU;
static uint8_t s_sentinel_dev_marker = 0xDDU;

static esp_err_t ceepew_oled_i2c_transmit(i2c_master_dev_handle_t dev_handle,
                                           const uint8_t *write_buffer,
                                           size_t write_size,
                                           int xfer_timeout_ms)
{
    /* dev_handle is a sentinel value; ignore it.  Route through the
     * Arduino Wire transport which works on this hardware where the
     * IDF I2C drivers NACK. */
    (void)dev_handle;
    s_i2c_tx_attempts++;
    (void)xfer_timeout_ms;
    esp_err_t rc = ceepew_oled_arduino_transmit(write_buffer, write_size);
    if (g_oled_in_init_stream) {
        ESP_LOGI(TAG, "[BOARD %02X] [OLED DIAG] [WIRE TX #%lu] size=%u rc=%d (%s)",
                 get_board_tag(), (unsigned long)s_i2c_tx_attempts,
                 (unsigned)write_size, (int)rc, esp_err_to_name(rc));
    }
    return rc;
}

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

#define CEEPEW_OLED_TILE_COLS  16U
#define CEEPEW_OLED_TILE_ROWS  8U

struct ceepew_oled_t {
    i2c_master_bus_handle_t  bus;
    i2c_master_dev_handle_t  i2c_dev;
    uint8_t                  addr;
    uint8_t                  buffer[CEEPEW_OLED_BUF_SIZE];
    bool                     sh1106_mode;
    bool                     initialised;
};

/* ── Logging tag ─────────────────────────────────────────────────── */

/* TAG defined early at the top of the file */

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
    // Static device is used, size is verified by compile-time layout.

    (void)memset(&s_dev, 0, sizeof(s_dev));
    s_dev.bus         = NULL;
    s_dev.i2c_dev     = NULL;
    s_dev.addr        = 0U;
    s_dev.sh1106_mode = false;
    s_dev.initialised = false;
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
    dev->bus         = NULL;
    dev->i2c_dev     = NULL;
    dev->addr        = 0U;
    dev->sh1106_mode = false;
    dev->initialised = false;
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

/* ── I2C bus init (Arduino Wire transport) ────────────────────────── */

esp_err_t ceepew_oled_bus_init(i2c_master_bus_handle_t *out_bus,
                               i2c_master_dev_handle_t *out_dev,
                               gpio_num_t sda, gpio_num_t scl,
                               uint32_t speed_hz,
                               uint8_t addr)
{
    assert(out_bus != NULL && out_dev != NULL);
    *out_bus = NULL;
    *out_dev = NULL;

    /* ---------------------------------------------------------------
     * TRANSPORT: Arduino Wire library (register-level I2C HAL).
     *
     * Both the IDF driver-ng (i2c_master_transmit) and legacy driver
     * (i2c_master_write_to_device) NACK on data-phase bytes for this
     * OLED clone.  The Arduino Wire library, which uses the low-level
     * ESP32 I2C HAL (register-level), works on the same hardware.
     * See DEBUG_LOG_OLED_I2C.md, Tests 001-007.
     * --------------------------------------------------------------- */

    (void)sda;
    (void)scl;
    (void)addr;

    /* Raw GPIO pre-init — confirm bus lines are pulled HIGH */
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    gpio_set_direction(sda, GPIO_MODE_INPUT);
    gpio_set_direction(scl, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(2));
    int sda_lvl = gpio_get_level(sda);
    int scl_lvl = gpio_get_level(scl);
    ESP_LOGI(TAG, "[BOARD %02X] [OLED DIAG] GPIO pre-init: SDA(GPIO%d)=%d, SCL(GPIO%d)=%d",
             get_board_tag(), (int)sda, sda_lvl, (int)scl, scl_lvl);

    /* Initialise the Arduino Wire transport on GPIO26/27 at requested frequency */
    esp_err_t rc = ceepew_oled_arduino_init(speed_hz);
    ESP_LOGI(TAG, "[BOARD %02X] [OLED DIAG] ceepew_oled_arduino_init (SDA=GPIO%d, SCL=GPIO%d, freq=%lu) returned: %d (%s)",
             get_board_tag(), (int)sda, (int)scl,
             (unsigned long)speed_hz, (int)rc, esp_err_to_name(rc));
    if (rc != ESP_OK) {
        CEEPEW_LOG(TAG, "OLED Arduino init failed: esp_err %d (%s)", rc, esp_err_to_name(rc));
        return rc;
    }

    /* Return sentinel non-NULL handles so that callers (hal_ui.c) that
     * check `bus != NULL` and `dev != NULL` are satisfied.  These pointers
     * are NEVER dereferenced; the actual I2C is done via the Arduino Wire
     * transport. */
    *out_bus = (i2c_master_bus_handle_t)&s_sentinel_bus_marker;
    *out_dev = (i2c_master_dev_handle_t)&s_sentinel_dev_marker;

    vTaskDelay(pdMS_TO_TICKS(10U));  /* allow Wire to stabilise */
    return ESP_OK;
}

/**
 * @brief Safe cleanup of bus/dev handles.
 *
 * With the Arduino Wire transport the bus and dev are sentinel pointers
 * pointing into static storage — they must NEVER be passed to
 * i2c_master_bus_rm_device or i2c_del_master_bus, which would dereference
 * them as real driver-ng handles and crash.  This function is a deliberate
 * no-op: the Arduino Wire transport is installed once at boot and lives
 * for the duration of the firmware run.
 */
void ceepew_oled_bus_cleanup(i2c_master_bus_handle_t bus,
                              i2c_master_dev_handle_t dev)
{
    /* Intentional no-op for the Arduino Wire transport sentinel handles. */
    (void)bus;
    (void)dev;
}

/* Helper functions for sending commands in short chunks (avoids signal issues / controller NACKs on streams) */
static esp_err_t send_cmd_1(i2c_master_dev_handle_t dev_handle, uint8_t cmd)
{
    uint8_t buf[2] = { CEEPEW_OLED_CTRL_CMD_STREAM, cmd };
    return ceepew_oled_i2c_transmit(dev_handle, buf, 2, CEEPEW_OLED_I2C_TIMEOUT_TICKS);
}

static esp_err_t send_cmd_2(i2c_master_dev_handle_t dev_handle, uint8_t cmd, uint8_t param)
{
    uint8_t buf[4] = { 0x80U, cmd, CEEPEW_OLED_CTRL_CMD_STREAM, param };
    return ceepew_oled_i2c_transmit(dev_handle, buf, 4, CEEPEW_OLED_I2C_TIMEOUT_TICKS);
}

/* ── SSD1306 init command stream ─────────────────────────────────── */

static esp_err_t send_init_stream(ceepew_oled_t *dev)
{
    assert(dev != NULL);
    /* dev->i2c_dev is a sentinel; actual I2C goes through legacy driver */

    /* Replicate the old working driver (ssd1306_i2c_new.c @ 8473afd):
     * send the entire init sequence as a single i2c_master_transmit burst
     * starting with control byte CEEPEW_OLED_CTRL_CMD_STREAM (0x00).
     * The old driver's 27-byte burst worked; the new approach of
     * sending each command separately fails on this OLED clone. */
    uint8_t cmd_buf[] = {
        CEEPEW_OLED_CTRL_CMD_STREAM,
        CEEPEW_OLED_CMD_DISPLAY_OFF,
        CEEPEW_OLED_CMD_SET_MUX_RATIO, 0x3FU,
        CEEPEW_OLED_CMD_SET_DISPLAY_OFFSET, 0x00U,
        CEEPEW_OLED_CMD_SET_DISPLAY_START_LINE,
        CEEPEW_OLED_CMD_SET_SEGMENT_REMAP_1,
        CEEPEW_OLED_CMD_SET_COM_SCAN_MODE,
        CEEPEW_OLED_CMD_SET_DISPLAY_CLK_DIV, 0x80U,
        CEEPEW_OLED_CMD_SET_COM_PIN_MAP, 0x12U,
        CEEPEW_OLED_CMD_SET_CONTRAST, 0xFFU,
        CEEPEW_OLED_CMD_DISPLAY_RAM,
        CEEPEW_OLED_CMD_SET_VCOMH_DESELCT, 0x40U,
        CEEPEW_OLED_CMD_SET_MEMORY_ADDR_MODE, 0x02U,
        CEEPEW_OLED_CMD_SET_LOWER_COL,
        CEEPEW_OLED_CMD_SET_HIGHER_COL,
        CEEPEW_OLED_CMD_SET_CHARGE_PUMP, 0x14U,
        CEEPEW_OLED_CMD_DEACTIVE_SCROLL,
        CEEPEW_OLED_CMD_DISPLAY_NORMAL,
        CEEPEW_OLED_CMD_DISPLAY_ON,
    };

    esp_err_t rc = ceepew_oled_i2c_transmit(dev->i2c_dev, cmd_buf, sizeof(cmd_buf),
                                            CEEPEW_OLED_I2C_TIMEOUT_TICKS);
    if (rc != ESP_OK) {
        CEEPEW_LOG(TAG, "OLED init burst failed: esp_err %d (%s)", rc, esp_err_to_name(rc));
        return rc;
    }

    ESP_LOGI(TAG, "[BOARD %02X] OLED panel configured for SSD1306 (128x64)", get_board_tag());
    vTaskDelay(pdMS_TO_TICKS(150U));
    return rc;
}

/* ── SH1106 init command stream ─────────────────────────────────── */

static esp_err_t send_init_stream_sh1106(ceepew_oled_t *dev)
{
    assert(dev != NULL);
    /* dev->i2c_dev is a sentinel; actual I2C goes through legacy driver */

    esp_err_t rc = ESP_OK;

    #define CHECK_RC(stage, expr) \
        do { \
            rc = (expr); \
            if (rc != ESP_OK) { \
                CEEPEW_LOG(TAG, "OLED init failed at stage %s: esp_err %d (%s)", stage, rc, esp_err_to_name(rc)); \
                return rc; \
            } \
        } while (0)

    CHECK_RC("init_stream_write_1", send_cmd_1(dev->i2c_dev, 0xAEU));  /* Display Off */
    CHECK_RC("init_stream_write_2", send_cmd_2(dev->i2c_dev, 0xD5U, 0x80U));  /* Set Display Clock Divide Ratio */
    CHECK_RC("init_stream_write_3", send_cmd_2(dev->i2c_dev, 0xA8U, 0x3FU));  /* Set Multiplex Ratio */
    CHECK_RC("init_stream_write_4", send_cmd_2(dev->i2c_dev, 0xD3U, 0x00U));  /* Set Display Offset */
    CHECK_RC("init_stream_write_5", send_cmd_1(dev->i2c_dev, 0x40U));  /* Set Display Start Line to 0 */
    CHECK_RC("init_stream_write_6", send_cmd_2(dev->i2c_dev, 0xADU, 0x8BU));  /* Set Charge Pump Command (Enable) */
    CHECK_RC("init_stream_write_7", send_cmd_1(dev->i2c_dev, 0xA0U));  /* Set Segment Re-map (direct: COL0=SEG0) */
    CHECK_RC("init_stream_write_8", send_cmd_1(dev->i2c_dev, 0xC8U));  /* Set COM Output Scan Direction */
    CHECK_RC("init_stream_write_9", send_cmd_2(dev->i2c_dev, 0xDAU, 0x12U));  /* Set COM Pins Hardware Configuration */
    CHECK_RC("init_stream_write_10", send_cmd_2(dev->i2c_dev, 0x81U, 0xFFU));  /* Set Contrast Control */
    CHECK_RC("init_stream_write_11", send_cmd_2(dev->i2c_dev, 0xD9U, 0x22U));  /* Set Pre-charge Period */
    CHECK_RC("init_stream_write_12", send_cmd_2(dev->i2c_dev, 0xDBU, 0x35U));  /* Set VCOMH Deselect Level */
    CHECK_RC("init_stream_write_13", send_cmd_1(dev->i2c_dev, 0xA4U));  /* Entire Display ON */
    CHECK_RC("init_stream_write_14", send_cmd_1(dev->i2c_dev, 0xA6U));  /* Set Normal Display */
    CHECK_RC("init_stream_write_15", send_cmd_1(dev->i2c_dev, 0xAFU));  /* Set Display ON */

    #undef CHECK_RC

    ESP_LOGI(TAG, "[BOARD %02X] OLED panel configured for SH1106 (128x64)", get_board_tag());
    vTaskDelay(pdMS_TO_TICKS(150U));
    return rc;
}

/* ── Fast-mode probe (stub — not used with Arduino Wire transport) ── */

bool ceepew_oled_probe_fast_mode(i2c_master_bus_handle_t bus, uint8_t addr)
{
    (void)bus;
    (void)addr;
    /* Fast-mode probe is not supported with the Arduino Wire transport.
     * The Wire transport runs at a fixed 400 kHz. */
    return false;
}

/* ── Init panel ──────────────────────────────────────────────────── */

esp_err_t ceepew_oled_init_panel(ceepew_oled_t *dev,
                                 i2c_master_bus_handle_t bus,
                                 i2c_master_dev_handle_t i2c_dev,
                                 uint8_t addr)
{
    assert(dev != NULL);
    /* i2c_dev is a sentinel from ceepew_oled_bus_init(); OK to be non-NULL */
    assert(i2c_dev != NULL);

    dev->bus         = bus;
    dev->i2c_dev     = i2c_dev;
    dev->addr        = addr;
    dev->sh1106_mode = false;
    (void)memset(dev->buffer, 0, CEEPEW_OLED_BUF_SIZE);

#ifdef CONFIG_CEEPEW_OLED_FORCE_SH1106
    esp_err_t rc = send_init_stream_sh1106(dev);
    if (rc == ESP_OK) {
        dev->sh1106_mode = true;
        dev->initialised = true;
    }
#else
    /* VDD has been stable for hundreds of ms by this point (ESP32 boot
     * time). Send the full init stream immediately — starts with 0xAE
     * (Display OFF) to hide power-on GDDRAM garbage. */
    esp_err_t rc = send_init_stream(dev);
    if (rc == ESP_OK) {
        dev->initialised = true;
    }
#endif
    return rc;
}

esp_err_t ceepew_oled_init_panel_sh1106(ceepew_oled_t *dev,
                                        i2c_master_bus_handle_t bus,
                                        i2c_master_dev_handle_t i2c_dev,
                                        uint8_t addr)
{
    assert(dev != NULL);
    /* i2c_dev is a sentinel from ceepew_oled_bus_init(); OK to be non-NULL */
    assert(i2c_dev != NULL);

    dev->bus         = bus;
    dev->i2c_dev     = i2c_dev;
    dev->addr        = addr;
    dev->sh1106_mode = true;
    (void)memset(dev->buffer, 0, CEEPEW_OLED_BUF_SIZE);

    esp_err_t rc = send_init_stream_sh1106(dev);
    if (rc == ESP_OK) {
        dev->initialised = true;
    }
    return rc;
}

/* ── Display push: SSD1306 fast path ─────────────────────────────── */

static esp_err_t push_full_frame(ceepew_oled_t *dev,
                                 i2c_master_dev_handle_t dev_handle)
{
    /* SH1106 uses 0xA0 (direct segment mapping), so frame buffer column
     * 0 maps directly to GDDRAM column 0 → SEG0 (leftmost pixel).
     * No column offset needed — unlike the old 0xA1 remap which required
     * shifting to columns 4-131 to hit the 128 visible segments. */
    const uint8_t col_offset = 0U;

    for (uint8_t page = 0U; page < CEEPEW_OLED_PAGES; page++) {
        const uint8_t page_cmd = (uint8_t)(CEEPEW_OLED_CMD_SET_PAGE_START | page);
        const uint8_t col_low  = (uint8_t)(col_offset & 0x0FU);
        const uint8_t col_high = (uint8_t)(0x10U | ((col_offset >> 4U) & 0x0FU));

        const uint8_t cmd_stream[6U] = {
            0x80U, col_low,
            0x80U, col_high,
            0x80U, page_cmd,
        };
        esp_err_t rc = ceepew_oled_i2c_transmit(dev_handle, cmd_stream,
                                           sizeof(cmd_stream),
                                           CEEPEW_OLED_I2C_TIMEOUT_TICKS);
        if (rc != ESP_OK) {
            return rc;
        }

        static uint8_t s_page_stream[1U + CEEPEW_OLED_WIDTH_PX];
        s_page_stream[0U] = CEEPEW_OLED_CTRL_DATA_STREAM;
        (void)memcpy(&s_page_stream[1U],
                     &dev->buffer[(uint16_t)page * CEEPEW_OLED_WIDTH_PX],
                     CEEPEW_OLED_WIDTH_PX);

        rc = ceepew_oled_i2c_transmit(dev_handle, s_page_stream,
                                 sizeof(s_page_stream),
                                 CEEPEW_OLED_I2C_TIMEOUT_TICKS);
        if (rc != ESP_OK) {
            return rc;
        }
    }
    return ESP_OK;
}

esp_err_t ceepew_oled_display(ceepew_oled_t *dev)
{
    assert(dev != NULL);
    assert(dev->initialised);
    assert(dev->i2c_dev != NULL);

    esp_err_t rc = push_full_frame(dev, dev->i2c_dev);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Display push failed: %d (%s)",
                 (int)rc, esp_err_to_name(rc));
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

        const uint8_t cmd_stream[6U] = {
            0x80U, col_low,
            0x80U, col_high,
            0x80U, page_cmd,
        };
        esp_err_t rc = ceepew_oled_i2c_transmit(dev->i2c_dev, cmd_stream,
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

        rc = ceepew_oled_i2c_transmit(dev->i2c_dev, data_stream,
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

    /* SH1106 uses 0xA0 (direct mapping), no column adjustment needed. */
    const uint8_t col_adjust = 0U;
    const uint8_t col_start  = (uint8_t)(tile_col * 8U + col_adjust);
    const uint8_t col_low    = (uint8_t)(col_start & 0x0FU);
    const uint8_t col_high   = (uint8_t)(0x10U | ((col_start >> 4U) & 0x0FU));
    i2c_master_dev_handle_t dev_handle = dev->i2c_dev;

    for (uint8_t page = 0U; page < CEEPEW_OLED_PAGES; page++) {
        const uint8_t page_cmd = (uint8_t)(CEEPEW_OLED_CMD_SET_PAGE_START | page);
        const uint8_t cmd[6U] = {
            0x80U, col_low,
            0x80U, col_high,
            0x80U, page_cmd,
        };
        esp_err_t rc = ceepew_oled_i2c_transmit(dev_handle, cmd,
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
        rc = ceepew_oled_i2c_transmit(dev_handle, data, sizeof(data),
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

    const uint8_t cmd[4U] = {
        0x80U,
        CEEPEW_OLED_CMD_SET_CONTRAST,
        0x80U,
        contrast,
    };
    return ceepew_oled_i2c_transmit(dev->i2c_dev, cmd, sizeof(cmd),
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
    return ceepew_oled_i2c_transmit(dev->i2c_dev, cmd, sizeof(cmd),
                               CEEPEW_OLED_I2C_TIMEOUT_TICKS);
}

void ceepew_oled_bus_recover(gpio_num_t sda, gpio_num_t scl)
{
    ESP_LOGI(TAG, "[BOARD %02X] [OLED DIAG] Starting bus recovery on SDA (GPIO%d), SCL (GPIO%d)",
             get_board_tag(), (int)sda, (int)scl);
             
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << sda);
    io_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&io_conf);

    vTaskDelay(pdMS_TO_TICKS(10));

    int sda_init_level = gpio_get_level(sda);
    if (sda_init_level == 1) {
        ESP_LOGI(TAG, "[BOARD %02X] [OLED DIAG] SDA is already HIGH, no recovery needed", get_board_tag());
        return;
    }

    ESP_LOGW(TAG, "[BOARD %02X] [OLED DIAG] SDA is LOW (wedged). Bit-banging SCL to recover...", get_board_tag());

    for (int i = 0; i < 9; i++) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(5);
        
        if (gpio_get_level(sda) == 1) {
            ESP_LOGI(TAG, "[BOARD %02X] [OLED DIAG] SDA released to HIGH after %d SCL pulses", get_board_tag(), i + 1);
            break;
        }
    }

    io_conf.pin_bit_mask = (1ULL << sda);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    gpio_config(&io_conf);
    
    gpio_set_level(sda, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(5);

    ESP_LOGI(TAG, "[BOARD %02X] [OLED DIAG] Bus recovery complete (SCL pulses + STOP sent)", get_board_tag());

    /* Release pins back to default state so the I2C driver can claim them */
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    vTaskDelay(pdMS_TO_TICKS(10));
}

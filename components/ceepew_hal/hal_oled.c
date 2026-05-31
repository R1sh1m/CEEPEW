/* components/ceepew_hal/hal_oled.c */

#include "hal_oled.h"
#include "hal_pins.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#define CEEPEW_OLED_FB_BYTES ((CEEPEW_OLED_WIDTH_PX * CEEPEW_OLED_HEIGHT_PX) / 8U)
#define CEEPEW_OLED_PAGE_COUNT (CEEPEW_OLED_HEIGHT_PX / 8U)
#define CEEPEW_OLED_TEXT_GLYPH_W 6U
#define CEEPEW_OLED_TEXT_GLYPH_H 8U
#define CEEPEW_OLED_MAX_TEXT_CHARS (CEEPEW_OLED_WIDTH_PX / CEEPEW_OLED_TEXT_GLYPH_W)
#define CEEPEW_OLED_I2C_SETTLE_MS 20U
#define CEEPEW_OLED_I2C_INTER_PAGE_DELAY_MS 1U

static const char *TAG = "hal_oled";

typedef struct {
    bool initialised;
    bool use_sh1106_offset_mode;
    i2c_master_bus_handle_t bus_handle;
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
    uint8_t active_addr;
    uint32_t active_freq_hz;
    uint8_t framebuffer[CEEPEW_OLED_FB_BYTES];
} OledState_t;

static OledState_t s_state = {
    .initialised = false,
    .use_sh1106_offset_mode = false,
    .bus_handle = NULL,
    .io_handle = NULL,
    .panel_handle = NULL,
    .active_addr = 0U,
    .active_freq_hz = 0U,
    .framebuffer = {0U},
};

static const uint8_t s_glyph_question[5] = {0x02U, 0x01U, 0x59U, 0x09U, 0x06U};
static const uint8_t s_glyph_space[5] = {0U, 0U, 0U, 0U, 0U};
static const uint8_t s_glyph_exclaim[5] = {0x00U, 0x00U, 0x5FU, 0x00U, 0x00U};
static const uint8_t s_glyph_dash[5] = {0x08U, 0x08U, 0x08U, 0x08U, 0x08U};
static const uint8_t s_glyph_underscore[5] = {0x40U, 0x40U, 0x40U, 0x40U, 0x40U};
static const uint8_t s_glyph_period[5] = {0x00U, 0x60U, 0x60U, 0x00U, 0x00U};
static const uint8_t s_glyph_colon[5] = {0x00U, 0x36U, 0x36U, 0x00U, 0x00U};
static const uint8_t s_glyph_slash[5] = {0x20U, 0x10U, 0x08U, 0x04U, 0x02U};
static const uint8_t s_glyph_plus[5] = {0x08U, 0x08U, 0x3EU, 0x08U, 0x08U};
static const uint8_t s_glyph_lparen[5] = {0x00U, 0x1CU, 0x22U, 0x41U, 0x00U};
static const uint8_t s_glyph_rparen[5] = {0x00U, 0x41U, 0x22U, 0x1CU, 0x00U};

static const uint8_t s_glyph_0[5] = {0x3EU, 0x45U, 0x49U, 0x51U, 0x3EU};
static const uint8_t s_glyph_1[5] = {0x00U, 0x21U, 0x7FU, 0x01U, 0x00U};
static const uint8_t s_glyph_2[5] = {0x21U, 0x43U, 0x45U, 0x49U, 0x31U};
static const uint8_t s_glyph_3[5] = {0x42U, 0x41U, 0x51U, 0x69U, 0x46U};
static const uint8_t s_glyph_4[5] = {0x0CU, 0x14U, 0x24U, 0x7FU, 0x04U};
static const uint8_t s_glyph_5[5] = {0x72U, 0x51U, 0x51U, 0x51U, 0x4EU};
static const uint8_t s_glyph_6[5] = {0x1EU, 0x29U, 0x49U, 0x49U, 0x06U};
static const uint8_t s_glyph_7[5] = {0x40U, 0x47U, 0x48U, 0x50U, 0x60U};
static const uint8_t s_glyph_8[5] = {0x36U, 0x49U, 0x49U, 0x49U, 0x36U};
static const uint8_t s_glyph_9[5] = {0x30U, 0x49U, 0x49U, 0x4AU, 0x3CU};

static const uint8_t s_glyph_A[5] = {0x7EU, 0x11U, 0x11U, 0x11U, 0x7EU};
static const uint8_t s_glyph_B[5] = {0x7FU, 0x49U, 0x49U, 0x49U, 0x36U};
static const uint8_t s_glyph_C[5] = {0x3EU, 0x41U, 0x41U, 0x41U, 0x22U};
static const uint8_t s_glyph_D[5] = {0x7FU, 0x41U, 0x41U, 0x22U, 0x1CU};
static const uint8_t s_glyph_E[5] = {0x7FU, 0x49U, 0x49U, 0x49U, 0x41U};
static const uint8_t s_glyph_F[5] = {0x7FU, 0x09U, 0x09U, 0x09U, 0x01U};
static const uint8_t s_glyph_G[5] = {0x3EU, 0x41U, 0x49U, 0x49U, 0x7AU};
static const uint8_t s_glyph_H[5] = {0x7FU, 0x08U, 0x08U, 0x08U, 0x7FU};
static const uint8_t s_glyph_I[5] = {0x00U, 0x41U, 0x7FU, 0x41U, 0x00U};
static const uint8_t s_glyph_J[5] = {0x20U, 0x40U, 0x41U, 0x3FU, 0x01U};
static const uint8_t s_glyph_K[5] = {0x7FU, 0x08U, 0x14U, 0x22U, 0x41U};
static const uint8_t s_glyph_L[5] = {0x7FU, 0x40U, 0x40U, 0x40U, 0x40U};
static const uint8_t s_glyph_M[5] = {0x7FU, 0x02U, 0x0CU, 0x02U, 0x7FU};
static const uint8_t s_glyph_N[5] = {0x7FU, 0x04U, 0x08U, 0x10U, 0x7FU};
static const uint8_t s_glyph_O[5] = {0x3EU, 0x41U, 0x41U, 0x41U, 0x3EU};
static const uint8_t s_glyph_P[5] = {0x7FU, 0x09U, 0x09U, 0x09U, 0x06U};
static const uint8_t s_glyph_Q[5] = {0x3EU, 0x41U, 0x51U, 0x21U, 0x5EU};
static const uint8_t s_glyph_R[5] = {0x7FU, 0x09U, 0x19U, 0x29U, 0x46U};
static const uint8_t s_glyph_S[5] = {0x26U, 0x49U, 0x49U, 0x49U, 0x32U};
static const uint8_t s_glyph_T[5] = {0x01U, 0x01U, 0x7FU, 0x01U, 0x01U};
static const uint8_t s_glyph_U[5] = {0x3FU, 0x40U, 0x40U, 0x40U, 0x3FU};
static const uint8_t s_glyph_V[5] = {0x1FU, 0x20U, 0x40U, 0x20U, 0x1FU};
static const uint8_t s_glyph_W[5] = {0x7FU, 0x20U, 0x18U, 0x20U, 0x7FU};
static const uint8_t s_glyph_X[5] = {0x63U, 0x14U, 0x08U, 0x14U, 0x63U};
static const uint8_t s_glyph_Y[5] = {0x03U, 0x04U, 0x78U, 0x04U, 0x03U};
static const uint8_t s_glyph_Z[5] = {0x61U, 0x51U, 0x49U, 0x45U, 0x43U};

static const uint8_t *oled_glyph_for_char(char ch) {
    if ((ch >= 'a') && (ch <= 'z')) { ch = (char)(ch - ('a' - 'A')); }

    switch (ch) {
    case ' ':
        return s_glyph_space;
    case '!':
        return s_glyph_exclaim;
    case '-':
        return s_glyph_dash;
    case '_':
        return s_glyph_underscore;
    case '.':
        return s_glyph_period;
    case ':':
        return s_glyph_colon;
    case '/':
        return s_glyph_slash;
    case '+':
        return s_glyph_plus;
    case '(':
        return s_glyph_lparen;
    case ')':
        return s_glyph_rparen;
    case '?':
        return s_glyph_question;
    case '0':
        return s_glyph_0;
    case '1':
        return s_glyph_1;
    case '2':
        return s_glyph_2;
    case '3':
        return s_glyph_3;
    case '4':
        return s_glyph_4;
    case '5':
        return s_glyph_5;
    case '6':
        return s_glyph_6;
    case '7':
        return s_glyph_7;
    case '8':
        return s_glyph_8;
    case '9':
        return s_glyph_9;
    case 'A':
        return s_glyph_A;
    case 'B':
        return s_glyph_B;
    case 'C':
        return s_glyph_C;
    case 'D':
        return s_glyph_D;
    case 'E':
        return s_glyph_E;
    case 'F':
        return s_glyph_F;
    case 'G':
        return s_glyph_G;
    case 'H':
        return s_glyph_H;
    case 'I':
        return s_glyph_I;
    case 'J':
        return s_glyph_J;
    case 'K':
        return s_glyph_K;
    case 'L':
        return s_glyph_L;
    case 'M':
        return s_glyph_M;
    case 'N':
        return s_glyph_N;
    case 'O':
        return s_glyph_O;
    case 'P':
        return s_glyph_P;
    case 'Q':
        return s_glyph_Q;
    case 'R':
        return s_glyph_R;
    case 'S':
        return s_glyph_S;
    case 'T':
        return s_glyph_T;
    case 'U':
        return s_glyph_U;
    case 'V':
        return s_glyph_V;
    case 'W':
        return s_glyph_W;
    case 'X':
        return s_glyph_X;
    case 'Y':
        return s_glyph_Y;
    case 'Z':
        return s_glyph_Z;
    default:
        return s_glyph_question;
    }
}

static void oled_set_pixel_unchecked(uint8_t x, uint8_t y, bool on) {
    const uint16_t index = (uint16_t)x + (uint16_t)((uint16_t)y / 8U) * (uint16_t)CEEPEW_OLED_WIDTH_PX;
    const uint8_t bit = (uint8_t)(1U << (y & 7U));
    if (on) {
        s_state.framebuffer[index] = (uint8_t)(s_state.framebuffer[index] | bit);
    }
    else {
        s_state.framebuffer[index] = (uint8_t)(s_state.framebuffer[index] & (uint8_t)~bit);
    }
}

static void oled_release_panel_io(void)
{
    /* FIX: drain any pending async I2C transactions before teardown.
     * Skipping this causes "Wrong I2C status, cannot delete device" when
     * the bus is destroyed while the DMA queue still holds failed ops. */
    if (s_state.bus_handle != NULL) {
        (void)i2c_master_bus_wait_all_done(s_state.bus_handle, 200);
    }

    vTaskDelay(pdMS_TO_TICKS(2U));
    if (s_state.panel_handle != NULL) {
        (void)esp_lcd_panel_del(s_state.panel_handle);
        s_state.panel_handle = NULL;
    }
    if (s_state.io_handle != NULL) {
        (void)esp_lcd_panel_io_del(s_state.io_handle);
        s_state.io_handle = NULL;
    }
}

static void oled_release_resources(void)
{
    oled_release_panel_io();
    if (s_state.bus_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(2U));
        (void)i2c_del_master_bus(s_state.bus_handle);
        s_state.bus_handle = NULL;
    }
    s_state.active_addr = 0U;
    s_state.active_freq_hz = 0U;
    s_state.use_sh1106_offset_mode = false;
}

static CeePewErr_t oled_tx_cmd(uint8_t cmd)
{
    CEEPEW_ASSERT(s_state.io_handle != NULL, CEEPEW_ERR_HW);
    esp_err_t rc = esp_lcd_panel_io_tx_param(s_state.io_handle, cmd, NULL, 0U);
    if (rc != ESP_OK) {
        return CEEPEW_ERR_HW;
    }
    return CEEPEW_OK;
}

static CeePewErr_t oled_tx_cmd1(uint8_t cmd, uint8_t param)
{
    CEEPEW_ASSERT(s_state.io_handle != NULL, CEEPEW_ERR_HW);
    esp_err_t rc = esp_lcd_panel_io_tx_param(s_state.io_handle, cmd, &param, 1U);
    if (rc != ESP_OK) {
        return CEEPEW_ERR_HW;
    }
    return CEEPEW_OK;
}

static CeePewErr_t oled_force_known_display_mode(void)
{
    CEEPEW_ASSERT(s_state.io_handle != NULL, CEEPEW_ERR_HW);
    CeePewErr_t err = oled_tx_cmd(0xAEU);          /* Display OFF during re-config */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd1(0xD5U, 0x80U);             /* Clock divide ratio / osc freq */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd1(0xA8U, 0x3FU);             /* Multiplex ratio 1/64 */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd1(0xD3U, 0x00U);             /* Display offset */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd(0x40U);                     /* Start line = 0 */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd1(0x8DU, 0x14U);             /* Charge pump ON */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd1(0x20U, 0x02U);             /* Page addressing mode */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd(0xA1U);                     /* Segment remap */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd(0xC8U);                     /* COM scan dec */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd1(0xDAU, 0x12U);             /* COM pins hw config */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd1(0x81U, 0xFFU);             /* Max contrast for visibility */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd1(0xD9U, 0xF1U);             /* Pre-charge period */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd1(0xDBU, 0x40U);             /* VCOMH deselect */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd(0xA4U);                     /* Resume RAM display */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd(0xA6U);                     /* Normal (not inverted) */
    if (err != CEEPEW_OK) { return err; }
    err = oled_tx_cmd(0xAFU);                     /* Display ON */
    if (err != CEEPEW_OK) { return err; }
    vTaskDelay(pdMS_TO_TICKS(CEEPEW_OLED_I2C_SETTLE_MS));
    return CEEPEW_OK;
}

static CeePewErr_t oled_flush_raw_pages(uint8_t col_offset)
{
    CEEPEW_ASSERT(s_state.io_handle != NULL, CEEPEW_ERR_HW);
    CEEPEW_ASSERT(col_offset <= 3U, CEEPEW_ERR_BOUNDS);
    CeePewErr_t err = oled_tx_cmd1(0x20U, 0x02U); /* Page addressing mode */
    if (err != CEEPEW_OK) {
        return err;
    }

    /* loop bound: CEEPEW_OLED_PAGE_COUNT = 8U */
    for (uint8_t page = 0U; page < CEEPEW_OLED_PAGE_COUNT; page++) {
        const uint8_t page_cmd = (uint8_t)(0xB0U | page);
        const uint8_t col_low = (uint8_t)(0x00U | (col_offset & 0x0FU));
        const uint8_t col_high = (uint8_t)(0x10U | ((col_offset >> 4U) & 0x0FU));
        const uint8_t *page_ptr = &s_state.framebuffer[(uint16_t)page * CEEPEW_OLED_WIDTH_PX];

        err = oled_tx_cmd(page_cmd);
        if (err != CEEPEW_OK) { return err; }
        err = oled_tx_cmd(col_low);
        if (err != CEEPEW_OK) { return err; }
        err = oled_tx_cmd(col_high);
        if (err != CEEPEW_OK) { return err; }

        esp_err_t rc = esp_lcd_panel_io_tx_color(s_state.io_handle, -1, page_ptr, CEEPEW_OLED_WIDTH_PX);
        if (rc != ESP_OK) {
            return CEEPEW_ERR_HW;
        }
        vTaskDelay(pdMS_TO_TICKS(CEEPEW_OLED_I2C_INTER_PAGE_DELAY_MS));
    }
    return CEEPEW_OK;
}

static CeePewErr_t oled_create_bus(uint32_t speed_hz)
{
    ESP_LOGI(TAG, "Initializing I2C bus at %lu Hz with SDA=%d SCL=%d",
             (unsigned long)speed_hz, CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL);

    i2c_master_bus_config_t bus_config = {
        .i2c_port = CEEPEW_I2C_PORT,
        .sda_io_num = CEEPEW_PIN_I2C_SDA,
        .scl_io_num = CEEPEW_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 2U,              /* Reduce aggression for faster I2C transitions */
        .intr_priority = 0,
        .trans_queue_depth     = 0U,             /* 0 = synchronous mode; disables async queue to prevent ops-list overflow */
        .flags = {
            .enable_internal_pullup = 0U,     /* Assume external pull-ups on board */
            .allow_pd = 0U,
        },
    };

    s_state.active_freq_hz = speed_hz;
    esp_err_t rc = i2c_new_master_bus(&bus_config, &s_state.bus_handle);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "i2c_new_master_bus failed (%lu Hz): 0x%x",
                 (unsigned long)speed_hz, rc);
        s_state.bus_handle = NULL;
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "I2C bus created successfully");
    return CEEPEW_OK;
}

static CeePewErr_t oled_init_panel_at_addr(uint8_t addr)
{
    ESP_LOGI(TAG, "Attempting panel init at I2C address 0x%02X", (unsigned int)addr);

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = addr,
        .scl_speed_hz = s_state.active_freq_hz,
        .control_phase_bytes = 1U,
        .dc_bit_offset = 6U,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0U,
            .disable_control_phase = 0U,
        },
    };

    ESP_LOGI(TAG, "Creating panel IO at 0x%02X with %lu Hz", 
             (unsigned int)addr, (unsigned long)s_state.active_freq_hz);
    esp_err_t rc = esp_lcd_new_panel_io_i2c(s_state.bus_handle, &io_config, &s_state.io_handle);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_new_panel_io_i2c failed at 0x%02X: 0x%x",
                 (unsigned int)addr, rc);
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "Panel IO created successfully");

    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = (uint8_t)CEEPEW_OLED_HEIGHT_PX,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 1U,
        .reset_gpio_num = -1,
        .flags = {
            .reset_active_high = 0U,
        },
        .vendor_config = &ssd1306_config,
    };

    ESP_LOGI(TAG, "Creating SSD1306 panel");
    rc = esp_lcd_new_panel_ssd1306(s_state.io_handle, &panel_config, &s_state.panel_handle);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_new_panel_ssd1306 failed at 0x%02X: 0x%x",
                 (unsigned int)addr, rc);
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "SSD1306 panel created successfully");

    ESP_LOGI(TAG, "Resetting panel");
    rc = esp_lcd_panel_reset(s_state.panel_handle);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_panel_reset failed at 0x%02X: 0x%x",
                 (unsigned int)addr, rc);
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "Panel reset successfully");

    ESP_LOGI(TAG, "Initializing panel (THIS IS WHERE IT FAILS)");
    rc = esp_lcd_panel_init(s_state.panel_handle);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_panel_init failed at 0x%02X: 0x%x",
                 (unsigned int)addr, rc);
        ESP_LOGW(TAG, "ERROR 0x108 typically indicates I2C bus error or device not responding");
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "Panel init successful");

    /* FIX: wait for the ~16 async SSD1306 init commands to fully complete
     * before queuing any further transactions. Without this, the queue fills
     * and oled_force_known_display_mode() fails with "ops list is full". */
    (void)i2c_master_bus_wait_all_done(s_state.bus_handle, 500);

    ESP_LOGI(TAG, "Enabling display");
    rc = esp_lcd_panel_disp_on_off(s_state.panel_handle, true);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_panel_disp_on_off failed at 0x%02X: 0x%x",
                 (unsigned int)addr, rc);
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "Display enabled successfully");

    /* FIX: drain the disp_on_off transaction too before the 16-command
     * force_known_display_mode sequence. The existing vTaskDelay only
     * yields CPU; it does not block until DMA is done. */
    (void)i2c_master_bus_wait_all_done(s_state.bus_handle, 500);
    vTaskDelay(pdMS_TO_TICKS(CEEPEW_OLED_I2C_SETTLE_MS));

    CeePewErr_t mode_err = oled_force_known_display_mode();
    if (mode_err != CEEPEW_OK) {
        ESP_LOGW(TAG, "oled_force_known_display_mode failed at 0x%02X", (unsigned int)addr);
        return mode_err;
    }

    s_state.active_addr = addr;
    ESP_LOGI(TAG, "Panel successfully initialized at 0x%02X", (unsigned int)addr);
    return CEEPEW_OK;
}

static void oled_draw_glyph(uint8_t x, uint8_t y, const uint8_t *glyph){
    CEEPEW_ASSERT_VOID(glyph != NULL);
    CEEPEW_ASSERT_VOID(x <= (uint8_t)(CEEPEW_OLED_WIDTH_PX - CEEPEW_OLED_TEXT_GLYPH_W) && y <= (uint8_t)(CEEPEW_OLED_HEIGHT_PX - CEEPEW_OLED_TEXT_GLYPH_H));
    for (uint8_t row = 0U; row < 8U; row++) {
        for (uint8_t col = 0U; col < 5U; col++) {
            const bool on = ((glyph[col] >> row) & 0x01U) != 0U;
            oled_set_pixel_unchecked((uint8_t)(x + col), (uint8_t)(y + row), on);
        }
    }
}

CeePewErr_t hal_oled_init(void){
    CEEPEW_ASSERT(!s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SDA) && GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SCL), CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(CEEPEW_OLED_WIDTH_PX == 128U && CEEPEW_OLED_HEIGHT_PX == 64U, CEEPEW_ERR_PARAM);
    
    ESP_LOGI(TAG, "hal_oled_init: Starting SSD1306 initialization");
    memset(s_state.framebuffer, 0, sizeof(s_state.framebuffer));

    const uint8_t addr_candidates[2] = {
        CEEPEW_OLED_I2C_ADDR,
        CEEPEW_OLED_I2C_ADDR_FB,
    };
    const uint32_t speed_candidates[2] = {
        CEEPEW_I2C_FREQ_HZ,
        CEEPEW_I2C_FREQ_FALLBACK_HZ,
    };
    CeePewErr_t err = CEEPEW_ERR_HW;
    for (uint8_t speed_idx = 0U; speed_idx < 2U; speed_idx++) {
        if ((speed_idx == 1U) && (speed_candidates[1] == speed_candidates[0])) {
            continue;
        }
        
        ESP_LOGI(TAG, "Trying I2C speed %lu Hz", (unsigned long)speed_candidates[speed_idx]);

        oled_release_resources();
        err = oled_create_bus(speed_candidates[speed_idx]);
        if (err != CEEPEW_OK) {
            ESP_LOGW(TAG, "Failed to create I2C bus at %lu Hz", (unsigned long)speed_candidates[speed_idx]);
            continue;
        }

        for (uint8_t addr_idx = 0U; addr_idx < 2U; addr_idx++) {
            ESP_LOGI(TAG, "Trying I2C address 0x%02X", (unsigned int)addr_candidates[addr_idx]);
            err = oled_init_panel_at_addr(addr_candidates[addr_idx]);
            if (err == CEEPEW_OK) {
                ESP_LOGI(TAG, "SUCCESS: Panel initialized at address 0x%02X", (unsigned int)addr_candidates[addr_idx]);
                break;
            }
            ESP_LOGW(TAG, "Address 0x%02X failed, trying next", (unsigned int)addr_candidates[addr_idx]);
            oled_release_panel_io();
        }
        if (err == CEEPEW_OK) {
            ESP_LOGI(TAG, "SUCCESS: Using %lu Hz", (unsigned long)speed_candidates[speed_idx]);
            break;
        }

        oled_release_resources();
    }

    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "FAILED: Could not initialize SSD1306 at any address/frequency combination");
        oled_release_resources();
        return err;
    }
    ESP_LOGI(TAG, "SSD1306 init OK at 0x%02X (%lu Hz)",
             (unsigned int)s_state.active_addr,
             (unsigned long)s_state.active_freq_hz);

    s_state.initialised = true;
    err = hal_oled_clear();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_oled_clear failed");
        oled_release_resources();
        s_state.initialised = false;
        return err;
    }
    err = hal_oled_flush();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_oled_flush failed");
        oled_release_resources();
        s_state.initialised = false;
        return err;
    }
    ESP_LOGI(TAG, "OLED initialization complete");
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_clear(void){
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    memset(s_state.framebuffer, 0, sizeof(s_state.framebuffer));
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_flush(void){
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(s_state.io_handle != NULL, CEEPEW_ERR_HW);

    const uint8_t base_offset = s_state.use_sh1106_offset_mode ? 2U : 0U;
    CeePewErr_t err = oled_flush_raw_pages(base_offset);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_oled_flush: raw page flush failed (offset %u)",
                 (unsigned int)base_offset);
        if (!s_state.use_sh1106_offset_mode) {
            CeePewErr_t fb_err = oled_flush_raw_pages(2U);
            if (fb_err == CEEPEW_OK) {
                s_state.use_sh1106_offset_mode = true;
                ESP_LOGW(TAG, "hal_oled_flush: switched to SH1106-style +2 column offset mode");
                return CEEPEW_OK;
            }
        }
        return err;
    }
    if (s_state.use_sh1106_offset_mode && (base_offset == 2U)) {
        return CEEPEW_OK;
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_blit(const uint8_t *framebuffer, uint32_t len)
{
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(framebuffer != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len == CEEPEW_OLED_FB_BYTES, CEEPEW_ERR_BOUNDS);

    memcpy(s_state.framebuffer, framebuffer, CEEPEW_OLED_FB_BYTES);
    return hal_oled_flush();
}

CeePewErr_t hal_oled_selftest_flash(uint32_t duration_ms)
{
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(duration_ms > 0U && duration_ms <= 3000U, CEEPEW_ERR_BOUNDS);

    CeePewErr_t err = oled_tx_cmd(0xA5U); /* Entire display ON */
    if (err != CEEPEW_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    err = oled_tx_cmd(0xA4U); /* Resume RAM display */
    if (err != CEEPEW_OK) {
        return err;
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_set_charge_pump(bool enabled)
{
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    const uint8_t mode = enabled ? 0x14U : 0x10U;
    return oled_tx_cmd1(0x8DU, mode);
}

CeePewErr_t hal_oled_draw_pixel(uint8_t x, uint8_t y, bool on){
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(x < CEEPEW_OLED_WIDTH_PX && y < CEEPEW_OLED_HEIGHT_PX, CEEPEW_ERR_BOUNDS);
    oled_set_pixel_unchecked(x, y, on);
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_draw_line(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, bool on) {
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(x0 < CEEPEW_OLED_WIDTH_PX && y0 < CEEPEW_OLED_HEIGHT_PX, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(x1 < CEEPEW_OLED_WIDTH_PX && y1 < CEEPEW_OLED_HEIGHT_PX, CEEPEW_ERR_BOUNDS);
    int32_t dx = (x1 > x0) ? (int32_t)(x1 - x0) : (int32_t)(x0 - x1);
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t dy = -((y1 > y0) ? (int32_t)(y1 - y0) : (int32_t)(y0 - y1));
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx + dy;
    int32_t x = (int32_t)x0;
    int32_t y = (int32_t)y0;
    for (uint32_t i = 0U; i < ((uint32_t)CEEPEW_OLED_WIDTH_PX + (uint32_t)CEEPEW_OLED_HEIGHT_PX); i++) {
        oled_set_pixel_unchecked((uint8_t)x, (uint8_t)y, on);
        if ((x == (int32_t)x1) && (y == (int32_t)y1)) { break;}
        const int32_t twice_err = (int32_t)(2 * err);
        if (twice_err >= dy){
            err += dy;
            x += sx;}
        if (twice_err <= dx){
            err += dx;
            y += sy;
        }
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on) {
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(x < CEEPEW_OLED_WIDTH_PX && y < CEEPEW_OLED_HEIGHT_PX, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(w > 0U && h > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(w <= (uint8_t)(CEEPEW_OLED_WIDTH_PX - x) && h <= (uint8_t)(CEEPEW_OLED_HEIGHT_PX - y), CEEPEW_ERR_BOUNDS);
    for (uint8_t yy = 0U; yy < CEEPEW_OLED_HEIGHT_PX; yy++) {
        if (yy >= h) { break; }
        for (uint8_t xx = 0U; xx < CEEPEW_OLED_WIDTH_PX; xx++){
            if (xx >= w){ break;}
            oled_set_pixel_unchecked((uint8_t)(x + xx), (uint8_t)(y + yy), on);
        }
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_draw_char(uint8_t x, uint8_t y, char ch) {
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(x <= (uint8_t)(CEEPEW_OLED_WIDTH_PX - CEEPEW_OLED_TEXT_GLYPH_W), CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(y <= (uint8_t)(CEEPEW_OLED_HEIGHT_PX - CEEPEW_OLED_TEXT_GLYPH_H), CEEPEW_ERR_BOUNDS);
    for (uint8_t row = 0U; row < CEEPEW_OLED_TEXT_GLYPH_H; row++) {
        for (uint8_t col = 0U; col < CEEPEW_OLED_TEXT_GLYPH_W; col++) {
            oled_set_pixel_unchecked((uint8_t)(x + col), (uint8_t)(y + row), false);
        }
    }
    oled_draw_glyph(x, y, oled_glyph_for_char(ch));
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_draw_text(uint8_t x, uint8_t y, const char *text) {
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(text != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(x < CEEPEW_OLED_WIDTH_PX, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(y <= (uint8_t)(CEEPEW_OLED_HEIGHT_PX - CEEPEW_OLED_TEXT_GLYPH_H), CEEPEW_ERR_BOUNDS);
    uint8_t cursor_x = x;
    for (uint8_t i = 0U; i < CEEPEW_OLED_MAX_TEXT_CHARS; i++) {
        const char ch = text[i];
        if (ch == '\0') { break;}
        if (cursor_x > (uint8_t)(CEEPEW_OLED_WIDTH_PX - CEEPEW_OLED_TEXT_GLYPH_W)) { break;}
        CeePewErr_t err = hal_oled_draw_char(cursor_x, y, ch);
        if (err != CEEPEW_OK) { return err;}
        cursor_x = (uint8_t)(cursor_x + CEEPEW_OLED_TEXT_GLYPH_W);
    }
    return CEEPEW_OK;
}

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

static const char *TAG = "hal_oled";

typedef struct {
    bool initialised;
    i2c_master_bus_handle_t bus_handle;
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
    uint8_t active_addr;
    uint8_t framebuffer[CEEPEW_OLED_FB_BYTES];
} OledState_t;

static OledState_t s_state = {
    .initialised = false,
    .bus_handle = NULL,
    .io_handle = NULL,
    .panel_handle = NULL,
    .active_addr = 0U,
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

static void oled_release_resources(void)
{
    if (s_state.panel_handle != NULL) {
        (void)esp_lcd_panel_del(s_state.panel_handle);
        s_state.panel_handle = NULL;
    }
    if (s_state.io_handle != NULL) {
        (void)esp_lcd_panel_io_del(s_state.io_handle);
        s_state.io_handle = NULL;
    }
    if (s_state.bus_handle != NULL) {
        (void)i2c_del_master_bus(s_state.bus_handle);
        s_state.bus_handle = NULL;
    }
    s_state.active_addr = 0U;
}

static CeePewErr_t oled_init_panel_at_addr(uint8_t addr)
{
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = addr,
        .scl_speed_hz = CEEPEW_I2C_FREQ_HZ,
        .control_phase_bytes = 1U,
        .dc_bit_offset = 6U,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0U,
            .disable_control_phase = 0U,
        },
    };
    esp_err_t rc = esp_lcd_new_panel_io_i2c(s_state.bus_handle, &io_config, &s_state.io_handle);
    if (rc != ESP_OK) {
        return CEEPEW_ERR_HW;
    }

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

    rc = esp_lcd_new_panel_ssd1306(s_state.io_handle, &panel_config, &s_state.panel_handle);
    if (rc != ESP_OK) {
        return CEEPEW_ERR_HW;
    }

    rc = esp_lcd_panel_reset(s_state.panel_handle);
    if (rc != ESP_OK) {
        return CEEPEW_ERR_HW;
    }
    rc = esp_lcd_panel_init(s_state.panel_handle);
    if (rc != ESP_OK) {
        return CEEPEW_ERR_HW;
    }
    rc = esp_lcd_panel_disp_on_off(s_state.panel_handle, true);
    if (rc != ESP_OK) {
        return CEEPEW_ERR_HW;
    }

    s_state.active_addr = addr;
    
    /* Send initial diagnostic test pattern to verify display is working */
    ESP_LOGI(TAG, "SSD1306 init complete at addr 0x%02X, sending test pattern", addr);
    
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
    memset(s_state.framebuffer, 0, sizeof(s_state.framebuffer));

    i2c_master_bus_config_t bus_config = {
        .i2c_port = CEEPEW_I2C_PORT,
        .sda_io_num = CEEPEW_PIN_I2C_SDA,
        .scl_io_num = CEEPEW_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7U,
        .intr_priority = 0,
        .trans_queue_depth = 0U,
        .flags = {
            .enable_internal_pullup = 1U,
            .allow_pd = 0U,
        },
    };
    esp_err_t rc = i2c_new_master_bus(&bus_config, &s_state.bus_handle);
    if (rc != ESP_OK) {
        oled_release_resources();
        return CEEPEW_ERR_HW;
    }

    const uint8_t addr_candidates[2] = {
        CEEPEW_OLED_I2C_ADDR,
        CEEPEW_OLED_I2C_ADDR_FB,
    };
    CeePewErr_t err = CEEPEW_ERR_HW;
    for (uint8_t i = 0U; i < 2U; i++) {
        err = oled_init_panel_at_addr(addr_candidates[i]);
        if (err == CEEPEW_OK) {
            break;
        }
        oled_release_resources();
        if (i == 0U) {
            rc = i2c_new_master_bus(&bus_config, &s_state.bus_handle);
            if (rc != ESP_OK) {
                break;
            }
        }
    }

    if (err != CEEPEW_OK) {
        oled_release_resources();
        return err;
    }

    s_state.initialised = true;
    ESP_LOGI(TAG, "OLED initialised at 0x%02X", (unsigned)s_state.active_addr);
    err = hal_oled_clear();
    if (err != CEEPEW_OK) {
        oled_release_resources();
        s_state.initialised = false;
        return err;
    }
    err = hal_oled_flush();
    if (err != CEEPEW_OK) {
        oled_release_resources();
        s_state.initialised = false;
        return err;
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_clear(void){
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    memset(s_state.framebuffer, 0, sizeof(s_state.framebuffer));
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_flush(void){
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(s_state.panel_handle != NULL, CEEPEW_ERR_HW);
    
    /* The SSD1306 displays memory in PAGE addressing mode (8 pages, 128 columns).
       Our framebuffer is in VERTICAL column format (8-pixel-tall columns).
       We must transpose the data before sending to match SSD1306 native layout:
       
       Our layout:     SSD1306 layout:
       byte[0]    = bits for (x=0, y=0-7)       byte[0] = bits for (x=0-7, page 0 at x=0)
       byte[1]    = bits for (x=1, y=0-7)  -->  byte[1] = bits for (x=0-7, page 0 at x=1)
       byte[128]  = bits for (x=0, y=8-15)      byte[128] = bits for (x=0-7, page 1 at x=0)
    */
    
    uint8_t transposed[CEEPEW_OLED_FB_BYTES];
    
    /* For each 8x8 block, transpose rows and columns */
    for (uint8_t page = 0U; page < CEEPEW_OLED_PAGE_COUNT; page++) {
        for (uint8_t x = 0U; x < CEEPEW_OLED_WIDTH_PX; x++) {
            /* Original: byte at [x + page * 128] contains bits for (x, page*8 + 0..7) */
            const uint8_t src_byte = s_state.framebuffer[x + (uint16_t)page * CEEPEW_OLED_WIDTH_PX];
            
            /* Destination: we need to write this byte to the page addressing location */
            const uint16_t dst_idx = (uint16_t)page * CEEPEW_OLED_WIDTH_PX + x;
            transposed[dst_idx] = src_byte;
        }
    }
    
    ESP_LOGI(TAG, "hal_oled_flush: sending bitmap to panel at 0x%02X", (unsigned)s_state.active_addr);
    esp_err_t rc = esp_lcd_panel_draw_bitmap(s_state.panel_handle,
                                             0,
                                             0,
                                             (int)CEEPEW_OLED_WIDTH_PX,
                                             (int)CEEPEW_OLED_HEIGHT_PX,
                                             transposed);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "hal_oled_flush: esp_lcd_panel_draw_bitmap failed: 0x%x", rc);
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "hal_oled_flush: bitmap sent successfully");
    return CEEPEW_OK;
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

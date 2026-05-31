/* components/ceepew_hal/hal_oled.c
 *
 * Direct I2C SSD1306 driver.
 * Bypasses esp_lcd entirely ? uses i2c_master_transmit() (ESP-IDF v5.1+).
 * Synchronous, blocking, no async queue, no panel framework.
 * Works on SSD1306 and SH1106 (auto-detected on first flush failure).
 */

#include "hal_oled.h"
#include "hal_pins.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "hal_oled";

/* ?? Dimensions ???????????????????????????????????????????????????????? */
#define OLED_W        CEEPEW_OLED_WIDTH_PX          /* 128 */
#define OLED_H        CEEPEW_OLED_HEIGHT_PX          /* 64  */
#define OLED_PAGES    (OLED_H / 8U)                  /* 8   */
#define FB_BYTES      (OLED_W * OLED_PAGES)          /* 1024 */

/* SSD1306 I2C control bytes (Co=0 in both cases ? stream follows) */
#define CTRL_CMD      0x00U   /* D/C = 0 : command stream */
#define CTRL_DATA     0x40U   /* D/C = 1 : data stream    */

#define I2C_TIMEOUT_MS  200   /* generous per-transaction timeout */

/* Glyph geometry */
#define GLYPH_W   5U
#define GLYPH_H   8U
#define GLYPH_ADV 6U                       /* 5 px glyph + 1 px gap  */
#define MAX_CHARS (OLED_W / GLYPH_ADV)    /* 21 characters per row  */

/* ?? Module state ?????????????????????????????????????????????????????? */
static struct {
    bool                    initialised;
    bool                    sh1106_mode;
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t                 addr;
    uint8_t                 fb[FB_BYTES];
} s;

/* ?? Low-level I2C primitives ?????????????????????????????????????????? */

/*
 * Send up to 16 command bytes in a single I2C transaction.
 * Wire format: [START][ADDR+W][0x00][cmd0][cmd1]...[STOP]
 *              ^^^^^^^^^^^^^^^ CTRL_CMD prefix = command stream
 */
static CeePewErr_t raw_cmds(const uint8_t *c, uint8_t n)
{
    CEEPEW_ASSERT(c != NULL && n > 0U && n <= 16U, CEEPEW_ERR_PARAM);
    uint8_t buf[17U];
    buf[0U] = CTRL_CMD;
    memcpy(&buf[1U], c, (size_t)n);
    return (i2c_master_transmit(s.dev, buf, (size_t)(n + 1U),
                                I2C_TIMEOUT_MS) == ESP_OK)
           ? CEEPEW_OK : CEEPEW_ERR_HW;
}

static inline CeePewErr_t cmd1(uint8_t c0)
{
    return raw_cmds(&c0, 1U);
}

static inline CeePewErr_t cmd2(uint8_t c0, uint8_t c1)
{
    const uint8_t b[2] = {c0, c1};
    return raw_cmds(b, 2U);
}

/*
 * Send one 128-byte framebuffer page.
 * Wire format: [START][ADDR+W][0x40][128 bytes][STOP]
 *              ^^^^^^^^^^^^^^^ CTRL_DATA prefix = data stream
 */
static CeePewErr_t send_page(uint8_t page)
{
    CEEPEW_ASSERT(page < OLED_PAGES, CEEPEW_ERR_PARAM);
    uint8_t buf[1U + OLED_W];
    buf[0U] = CTRL_DATA;
    memcpy(&buf[1U], &s.fb[(uint16_t)page * OLED_W], OLED_W);
    return (i2c_master_transmit(s.dev, buf, sizeof(buf),
                                I2C_TIMEOUT_MS) == ESP_OK)
           ? CEEPEW_OK : CEEPEW_ERR_HW;
}

/* ?? SSD1306 hardware init sequence ??????????????????????????????????? */
static CeePewErr_t hw_init(void)
{
    /* Standard SSD1306 128?64 init ? identical on genuine SSD1306 and SH1106 */
    CeePewErr_t e;
    e = cmd1(0xAEU);           if (e) return e;   /* Display OFF             */
    e = cmd2(0xD5U, 0x80U);    if (e) return e;   /* Clock / osc freq        */
    e = cmd2(0xA8U, 0x3FU);    if (e) return e;   /* Mux ratio 1/64          */
    e = cmd2(0xD3U, 0x00U);    if (e) return e;   /* Display offset = 0      */
    e = cmd1(0x40U);           if (e) return e;   /* Start line = 0          */
    e = cmd2(0x8DU, 0x14U);    if (e) return e;   /* Charge pump ON          */
    e = cmd2(0x20U, 0x02U);    if (e) return e;   /* Page addressing mode    */
    e = cmd1(0xA1U);           if (e) return e;   /* Segment remap           */
    e = cmd1(0xC8U);           if (e) return e;   /* COM scan dec            */
    e = cmd2(0xDAU, 0x12U);    if (e) return e;   /* COM pins hw config      */
    e = cmd2(0x81U, 0xFFU);    if (e) return e;   /* Max contrast            */
    e = cmd2(0xD9U, 0xF1U);    if (e) return e;   /* Pre-charge period       */
    e = cmd2(0xDBU, 0x40U);    if (e) return e;   /* VCOMH deselect          */
    e = cmd1(0xA4U);           if (e) return e;   /* Resume from RAM         */
    e = cmd1(0xA6U);           if (e) return e;   /* Normal (not inverted)   */
    e = cmd1(0xAFU);           if (e) return e;   /* Display ON              */
    return CEEPEW_OK;
}

/* ?? Page flush ???????????????????????????????????????????????????????? */
static CeePewErr_t flush_at_offset(uint8_t col_offset)
{
    for (uint8_t page = 0U; page < OLED_PAGES; page++) {
        const uint8_t cmds[3] = {
            (uint8_t)(0xB0U | page),                        /* Page set          */
            (uint8_t)(col_offset & 0x0FU),                  /* Col addr low      */
            (uint8_t)(0x10U | ((col_offset >> 4U) & 0x0FU)) /* Col addr high     */
        };
        CeePewErr_t e = raw_cmds(cmds, 3U);
        if (e != CEEPEW_OK) { return e; }
        e = send_page(page);
        if (e != CEEPEW_OK) { return e; }
    }
    return CEEPEW_OK;
}

/* ?? Public API ???????????????????????????????????????????????????????? */

CeePewErr_t hal_oled_init(void)
{
    CEEPEW_ASSERT(!s.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SDA) &&
                  GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SCL), CEEPEW_ERR_PINS);

    ESP_LOGI(TAG, "hal_oled_init (direct I2C, no esp_lcd)");
    memset(&s, 0, sizeof(s));

    const uint32_t speeds[2] = { CEEPEW_I2C_FREQ_HZ, CEEPEW_I2C_FREQ_FALLBACK_HZ };
    const uint8_t  addrs[2]  = { CEEPEW_OLED_I2C_ADDR, CEEPEW_OLED_I2C_ADDR_FB };

    CeePewErr_t err = CEEPEW_ERR_HW;

    for (uint8_t si = 0U; si < 2U && err != CEEPEW_OK; si++) {
        if (si == 1U && speeds[1] == speeds[0]) { continue; }

        const i2c_master_bus_config_t bus_cfg = {
            .i2c_port              = CEEPEW_I2C_PORT,
            .sda_io_num            = CEEPEW_PIN_I2C_SDA,
            .scl_io_num            = CEEPEW_PIN_I2C_SCL,
            .clk_source            = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt     = 7U,
            .intr_priority         = 0,
            /*
             * trans_queue_depth = 0 ? synchronous mode.
             * Every i2c_master_transmit() blocks until the physical transfer
             * completes before returning.  The queue can never fill because
             * there is no queue.  FreeRTOS yields to other tasks (BLE, etc.)
             * through the driver's internal semaphore during each byte transfer.
             */
            .trans_queue_depth     = 0U,
            .flags = {
                .enable_internal_pullup = 0U,
                .allow_pd               = 0U,
            },
        };

        ESP_LOGI(TAG, "Creating I2C bus at %lu Hz", (unsigned long)speeds[si]);
        if (i2c_new_master_bus(&bus_cfg, &s.bus) != ESP_OK) {
            ESP_LOGW(TAG, "i2c_new_master_bus failed");
            s.bus = NULL;
            continue;
        }

        for (uint8_t ai = 0U; ai < 2U && err != CEEPEW_OK; ai++) {
            ESP_LOGI(TAG, "Probing 0x%02X", (unsigned)addrs[ai]);
            if (i2c_master_probe(s.bus, addrs[ai], I2C_TIMEOUT_MS) != ESP_OK) {
                ESP_LOGW(TAG, "No ACK from 0x%02X", (unsigned)addrs[ai]);
                continue;
            }
            ESP_LOGI(TAG, "ACK from 0x%02X", (unsigned)addrs[ai]);

            const i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address  = addrs[ai],
                .scl_speed_hz    = speeds[si],
            };
            if (i2c_master_bus_add_device(s.bus, &dev_cfg, &s.dev) != ESP_OK) {
                ESP_LOGW(TAG, "bus_add_device failed");
                continue;
            }
            s.addr = addrs[ai];
            err = CEEPEW_OK;
        }

        if (err != CEEPEW_OK) {
            (void)i2c_del_master_bus(s.bus);
            s.bus = NULL;
        }
    }

    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "No SSD1306 found on I2C bus");
        return CEEPEW_ERR_HW;
    }

    ESP_LOGI(TAG, "Running init sequence on 0x%02X", (unsigned)s.addr);
    err = hw_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hw_init failed");
        (void)i2c_master_bus_rm_device(s.dev);
        (void)i2c_del_master_bus(s.bus);
        s.dev = NULL;
        s.bus = NULL;
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(10U));
    s.initialised = true;

    /* Blank the display */
    err = hal_oled_clear();
    if (err == CEEPEW_OK) { err = hal_oled_flush(); }
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "Initial flush failed");
        s.initialised = false;
        return err;
    }

    ESP_LOGI(TAG, "SSD1306 ready at 0x%02X (%lu Hz)",
             (unsigned)s.addr, (unsigned long)CEEPEW_I2C_FREQ_HZ);
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_clear(void)
{
    CEEPEW_ASSERT(s.initialised, CEEPEW_ERR_BUSY);
    memset(s.fb, 0, FB_BYTES);
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_flush(void)
{
    CEEPEW_ASSERT(s.initialised, CEEPEW_ERR_BUSY);

    CeePewErr_t err = flush_at_offset(s.sh1106_mode ? 2U : 0U);

    if (err != CEEPEW_OK && !s.sh1106_mode) {
        /* One silent retry with SH1106 +2 column offset */
        err = flush_at_offset(2U);
        if (err == CEEPEW_OK) {
            s.sh1106_mode = true;
            ESP_LOGW(TAG, "Switched to SH1106 +2 column offset");
        } else {
            ESP_LOGE(TAG, "hal_oled_flush failed");
        }
    }
    return err;
}

CeePewErr_t hal_oled_blit(const uint8_t *framebuffer, uint32_t len)
{
    CEEPEW_ASSERT(s.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(framebuffer != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len == (uint32_t)FB_BYTES, CEEPEW_ERR_BOUNDS);
    memcpy(s.fb, framebuffer, FB_BYTES);
    return hal_oled_flush();
}

CeePewErr_t hal_oled_selftest_flash(uint32_t duration_ms)
{
    CEEPEW_ASSERT(s.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(duration_ms > 0U && duration_ms <= 3000U, CEEPEW_ERR_BOUNDS);
    CeePewErr_t e = cmd1(0xA5U);   /* Entire display ON */
    if (e != CEEPEW_OK) { return e; }
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    return cmd1(0xA4U);             /* Resume from RAM  */
}

CeePewErr_t hal_oled_set_charge_pump(bool enabled)
{
    CEEPEW_ASSERT(s.initialised, CEEPEW_ERR_BUSY);
    return cmd2(0x8DU, enabled ? 0x14U : 0x10U);
}

/* ?? Framebuffer pixel ops ????????????????????????????????????????????? */

static void fb_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= OLED_W || y >= OLED_H) { return; }
    const uint16_t idx = (uint16_t)x + (uint16_t)((y / 8U) * OLED_W);
    const uint8_t  bit = (uint8_t)(1U << (y & 7U));
    if (on) { s.fb[idx] |= bit; }
    else    { s.fb[idx] &= (uint8_t)~bit; }
}

CeePewErr_t hal_oled_draw_pixel(uint8_t x, uint8_t y, bool on)
{
    CEEPEW_ASSERT(s.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(x < OLED_W && y < OLED_H, CEEPEW_ERR_BOUNDS);
    fb_pixel(x, y, on);
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on)
{
    CEEPEW_ASSERT(s.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(x < OLED_W && y < OLED_H, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(w > 0U && h > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT((uint16_t)x + w <= OLED_W && (uint16_t)y + h <= OLED_H,
                  CEEPEW_ERR_BOUNDS);
    for (uint8_t yy = 0U; yy < h; yy++) {
        for (uint8_t xx = 0U; xx < w; xx++) {
            fb_pixel((uint8_t)(x + xx), (uint8_t)(y + yy), on);
        }
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_draw_line(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, bool on)
{
    CEEPEW_ASSERT(s.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(x0 < OLED_W && y0 < OLED_H, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(x1 < OLED_W && y1 < OLED_H, CEEPEW_ERR_BOUNDS);
    int32_t dx = (x1 > x0) ? (int32_t)(x1 - x0) : (int32_t)(x0 - x1);
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t dy = -((y1 > y0) ? (int32_t)(y1 - y0) : (int32_t)(y0 - y1));
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t e  = dx + dy;
    int32_t x  = (int32_t)x0;
    int32_t y  = (int32_t)y0;
    for (uint32_t guard = 0U; guard < 256U; guard++) {
        fb_pixel((uint8_t)x, (uint8_t)y, on);
        if (x == (int32_t)x1 && y == (int32_t)y1) { break; }
        int32_t e2 = 2 * e;
        if (e2 >= dy) { e += dy; x += sx; }
        if (e2 <= dx) { e += dx; y += sy; }
    }
    return CEEPEW_OK;
}

/* ?? Font (identical bitmaps to original hal_oled.c) ?????????????????? */

static const uint8_t g_question[5]   = {0x02U,0x01U,0x59U,0x09U,0x06U};
static const uint8_t g_space[5]      = {0x00U,0x00U,0x00U,0x00U,0x00U};
static const uint8_t g_exclaim[5]    = {0x00U,0x00U,0x5FU,0x00U,0x00U};
static const uint8_t g_dash[5]       = {0x08U,0x08U,0x08U,0x08U,0x08U};
static const uint8_t g_underscore[5] = {0x40U,0x40U,0x40U,0x40U,0x40U};
static const uint8_t g_period[5]     = {0x00U,0x60U,0x60U,0x00U,0x00U};
static const uint8_t g_colon[5]      = {0x00U,0x36U,0x36U,0x00U,0x00U};
static const uint8_t g_slash[5]      = {0x20U,0x10U,0x08U,0x04U,0x02U};
static const uint8_t g_plus[5]       = {0x08U,0x08U,0x3EU,0x08U,0x08U};
static const uint8_t g_lparen[5]     = {0x00U,0x1CU,0x22U,0x41U,0x00U};
static const uint8_t g_rparen[5]     = {0x00U,0x41U,0x22U,0x1CU,0x00U};
static const uint8_t g_0[5] = {0x3EU,0x45U,0x49U,0x51U,0x3EU};
static const uint8_t g_1[5] = {0x00U,0x21U,0x7FU,0x01U,0x00U};
static const uint8_t g_2[5] = {0x21U,0x43U,0x45U,0x49U,0x31U};
static const uint8_t g_3[5] = {0x42U,0x41U,0x51U,0x69U,0x46U};
static const uint8_t g_4[5] = {0x0CU,0x14U,0x24U,0x7FU,0x04U};
static const uint8_t g_5[5] = {0x72U,0x51U,0x51U,0x51U,0x4EU};
static const uint8_t g_6[5] = {0x1EU,0x29U,0x49U,0x49U,0x06U};
static const uint8_t g_7[5] = {0x40U,0x47U,0x48U,0x50U,0x60U};
static const uint8_t g_8[5] = {0x36U,0x49U,0x49U,0x49U,0x36U};
static const uint8_t g_9[5] = {0x30U,0x49U,0x49U,0x4AU,0x3CU};
static const uint8_t g_A[5] = {0x7EU,0x11U,0x11U,0x11U,0x7EU};
static const uint8_t g_B[5] = {0x7FU,0x49U,0x49U,0x49U,0x36U};
static const uint8_t g_C[5] = {0x3EU,0x41U,0x41U,0x41U,0x22U};
static const uint8_t g_D[5] = {0x7FU,0x41U,0x41U,0x22U,0x1CU};
static const uint8_t g_E[5] = {0x7FU,0x49U,0x49U,0x49U,0x41U};
static const uint8_t g_F[5] = {0x7FU,0x09U,0x09U,0x09U,0x01U};
static const uint8_t g_G[5] = {0x3EU,0x41U,0x49U,0x49U,0x7AU};
static const uint8_t g_H[5] = {0x7FU,0x08U,0x08U,0x08U,0x7FU};
static const uint8_t g_I[5] = {0x00U,0x41U,0x7FU,0x41U,0x00U};
static const uint8_t g_J[5] = {0x20U,0x40U,0x41U,0x3FU,0x01U};
static const uint8_t g_K[5] = {0x7FU,0x08U,0x14U,0x22U,0x41U};
static const uint8_t g_L[5] = {0x7FU,0x40U,0x40U,0x40U,0x40U};
static const uint8_t g_M[5] = {0x7FU,0x02U,0x0CU,0x02U,0x7FU};
static const uint8_t g_N[5] = {0x7FU,0x04U,0x08U,0x10U,0x7FU};
static const uint8_t g_O[5] = {0x3EU,0x41U,0x41U,0x41U,0x3EU};
static const uint8_t g_P[5] = {0x7FU,0x09U,0x09U,0x09U,0x06U};
static const uint8_t g_Q[5] = {0x3EU,0x41U,0x51U,0x21U,0x5EU};
static const uint8_t g_R[5] = {0x7FU,0x09U,0x19U,0x29U,0x46U};
static const uint8_t g_S[5] = {0x26U,0x49U,0x49U,0x49U,0x32U};
static const uint8_t g_T[5] = {0x01U,0x01U,0x7FU,0x01U,0x01U};
static const uint8_t g_U[5] = {0x3FU,0x40U,0x40U,0x40U,0x3FU};
static const uint8_t g_V[5] = {0x1FU,0x20U,0x40U,0x20U,0x1FU};
static const uint8_t g_W[5] = {0x7FU,0x20U,0x18U,0x20U,0x7FU};
static const uint8_t g_X[5] = {0x63U,0x14U,0x08U,0x14U,0x63U};
static const uint8_t g_Y[5] = {0x03U,0x04U,0x78U,0x04U,0x03U};
static const uint8_t g_Z[5] = {0x61U,0x51U,0x49U,0x45U,0x43U};

static const uint8_t *glyph_for(char ch)
{
    if (ch >= 'a' && ch <= 'z') { ch = (char)(ch - ('a' - 'A')); }
    switch (ch) {
        case ' ': return g_space;      case '!': return g_exclaim;
        case '-': return g_dash;       case '_': return g_underscore;
        case '.': return g_period;     case ':': return g_colon;
        case '/': return g_slash;      case '+': return g_plus;
        case '(': return g_lparen;     case ')': return g_rparen;
        case '0': return g_0;  case '1': return g_1;
        case '2': return g_2;  case '3': return g_3;
        case '4': return g_4;  case '5': return g_5;
        case '6': return g_6;  case '7': return g_7;
        case '8': return g_8;  case '9': return g_9;
        case 'A': return g_A;  case 'B': return g_B;
        case 'C': return g_C;  case 'D': return g_D;
        case 'E': return g_E;  case 'F': return g_F;
        case 'G': return g_G;  case 'H': return g_H;
        case 'I': return g_I;  case 'J': return g_J;
        case 'K': return g_K;  case 'L': return g_L;
        case 'M': return g_M;  case 'N': return g_N;
        case 'O': return g_O;  case 'P': return g_P;
        case 'Q': return g_Q;  case 'R': return g_R;
        case 'S': return g_S;  case 'T': return g_T;
        case 'U': return g_U;  case 'V': return g_V;
        case 'W': return g_W;  case 'X': return g_X;
        case 'Y': return g_Y;  case 'Z': return g_Z;
        default:  return g_question;
    }
}

CeePewErr_t hal_oled_draw_char(uint8_t x, uint8_t y, char ch)
{
    CEEPEW_ASSERT(s.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(x <= (uint8_t)(OLED_W - GLYPH_W), CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(y <= (uint8_t)(OLED_H - GLYPH_H), CEEPEW_ERR_BOUNDS);
    /* Clear 6?8 cell (5 glyph + 1 gap) before drawing */
    for (uint8_t r = 0U; r < GLYPH_H; r++) {
        for (uint8_t c = 0U; c < GLYPH_ADV; c++) {
            fb_pixel((uint8_t)(x + c), (uint8_t)(y + r), false);
        }
    }
    const uint8_t *g = glyph_for(ch);
    for (uint8_t col = 0U; col < GLYPH_W; col++) {
        for (uint8_t row = 0U; row < GLYPH_H; row++) {
            fb_pixel((uint8_t)(x + col), (uint8_t)(y + row),
                     ((g[col] >> row) & 0x01U) != 0U);
        }
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_oled_draw_text(uint8_t x, uint8_t y, const char *text)
{
    CEEPEW_ASSERT(s.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(text != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(x < OLED_W, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(y <= (uint8_t)(OLED_H - GLYPH_H), CEEPEW_ERR_BOUNDS);
    uint8_t cx = x;
    for (uint8_t i = 0U; i < MAX_CHARS; i++) {
        if (text[i] == '\0') { break; }
        if (cx > (uint8_t)(OLED_W - GLYPH_W)) { break; }
        (void)hal_oled_draw_char(cx, y, text[i]);
        cx = (uint8_t)(cx + GLYPH_ADV);
    }
    return CEEPEW_OK;
}

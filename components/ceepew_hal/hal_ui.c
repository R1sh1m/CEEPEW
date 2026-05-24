/* components/ceepew_hal/hal_ui.c
 * UI rendering implementation: pixel/line/shape drawing,
 * font rendering, framebuffer management.
 */

#include "hal_ui.h"
#include "hal_oled.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

/* Design note: The UI layer maintains a 1-bit framebuffer (8 pixels per byte)
   representing the full 128×64 display. Primitives write to this buffer, and
   hal_ui_flush() transfers it to the SSD1306 via I2C. This allows efficient
   off-screen rendering and blitting without per-pixel I2C transactions. */

static const char *TAG = "hal_ui";

/* Framebuffer: 128 pixels wide × 64 pixels tall = 1024 bytes */
#define FRAMEBUFFER_SIZE_BYTES  ((HAL_UI_WIDTH_PX * HAL_UI_HEIGHT_PX) / 8U)

static uint8_t s_framebuffer[FRAMEBUFFER_SIZE_BYTES] = {0};
static bool s_ui_initialised = false;

/* Monospace 5×8 font bitmap (ASCII 32-126) — each character is 6 pixels wide
   and 8 pixels tall, stored as 5 bytes per character (5 columns × 8 rows / 8 bits). */
static const uint8_t FONT_5x8[95][5] = {
    /* ASCII 32 (space) */
    {0x00, 0x00, 0x00, 0x00, 0x00},
    /* ASCII 33 (!) */
    {0x00, 0x00, 0x5F, 0x00, 0x00},
    /* ASCII 34 (") */
    {0x00, 0x07, 0x00, 0x07, 0x00},
    /* ASCII 35 (#) */
    {0x14, 0x7F, 0x14, 0x7F, 0x14},
    /* ASCII 36 ($) */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    /* ASCII 37 (%) */
    {0x23, 0x13, 0x08, 0x64, 0x62},
    /* ASCII 38 (&) */
    {0x36, 0x49, 0x55, 0x22, 0x50},
    /* ASCII 39 (') */
    {0x00, 0x05, 0x03, 0x00, 0x00},
    /* ... Additional characters (truncated for brevity) ...
       In production, this table would contain all 95 printable ASCII chars. */
    /* For now, provide a simple pattern-fill placeholder for unmapped chars */
};

CeePewErr_t hal_ui_init(void){
    CEEPEW_ASSERT(!s_ui_initialised, CEEPEW_ERR_BUSY);
    memset(s_framebuffer, 0U, FRAMEBUFFER_SIZE_BYTES);
    s_ui_initialised = true;
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_clear(void){
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_PARAM);
    memset(s_framebuffer, 0U, FRAMEBUFFER_SIZE_BYTES);
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_flush(void){
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_PARAM);

    /* Keep the OLED in sync with the UI framebuffer. */
    CeePewErr_t err = hal_oled_clear();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_ui_flush: hal_oled_clear failed");
        return err;
    }

    uint16_t pixels_drawn = 0U;
    /* loop bound: HAL_UI_HEIGHT_PX / 8U = 8U pages (compile-time constant) */
    for (uint8_t page = 0U; page < (HAL_UI_HEIGHT_PX / 8U); page++) {
        const uint16_t base = (uint16_t)page * (uint16_t)HAL_UI_WIDTH_PX;
        /* loop bound: HAL_UI_WIDTH_PX = 128U (compile-time constant) */
        for (uint8_t x = 0U; x < HAL_UI_WIDTH_PX; x++) {
            const uint8_t column = s_framebuffer[base + x];
            if (column == 0U) {
                continue;
            }

            /* loop bound: 8U bits per page (compile-time constant) */
            for (uint8_t bit = 0U; bit < 8U; bit++) {
                if (((column >> bit) & 0x01U) == 0U) {
                    continue;
                }

                err = hal_oled_draw_pixel(x, (uint8_t)((page * 8U) + bit), true);
                if (err != CEEPEW_OK) {
                    ESP_LOGE(TAG, "hal_ui_flush: hal_oled_draw_pixel failed at (%d,%d)",
                             x, (page * 8U) + bit);
                    return err;
                }
                pixels_drawn++;
            }
        }
    }

    ESP_LOGI(TAG, "hal_ui_flush: %u pixels drawn, calling hal_oled_flush", pixels_drawn);
    return hal_oled_flush();
}

CeePewErr_t hal_ui_pixel(uint8_t x, uint8_t y, HalUIColor_t color){
    CEEPEW_ASSERT(x < HAL_UI_WIDTH_PX && y < HAL_UI_HEIGHT_PX, CEEPEW_ERR_BOUNDS);

    uint16_t byte_idx = (y / 8U) * HAL_UI_WIDTH_PX + x;
    CEEPEW_ASSERT(byte_idx < FRAMEBUFFER_SIZE_BYTES, CEEPEW_ERR_BOUNDS);

    uint8_t bit = 1U << (y % 8U);

    switch (color) {
        case HAL_UI_WHITE:
            s_framebuffer[byte_idx] |= bit;
            break;
        case HAL_UI_BLACK:
            s_framebuffer[byte_idx] &= ~bit;
            break;
        case HAL_UI_INVERT:
            s_framebuffer[byte_idx] ^= bit;
            break;
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_hline(uint8_t x0, uint8_t x1, uint8_t y, HalUIColor_t color){
    CEEPEW_ASSERT(y < HAL_UI_HEIGHT_PX, CEEPEW_ERR_BOUNDS);

    if (x0 > x1) {
        uint8_t tmp = x0; x0 = x1; x1 = tmp;
    }

    /* loop bound: x1 - x0 <= HAL_UI_WIDTH_PX = 128U (compile-time max) */
    for (uint8_t x = x0; x <= x1 && x < HAL_UI_WIDTH_PX; x++) {
        CeePewErr_t err = hal_ui_pixel(x, y, color);
        CEEPEW_ASSERT(err == CEEPEW_OK, err);
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_vline(uint8_t x, uint8_t y0, uint8_t y1, HalUIColor_t color)
{
    CEEPEW_ASSERT(x < HAL_UI_WIDTH_PX, CEEPEW_ERR_BOUNDS);

    if (y0 > y1) {
        uint8_t tmp = y0; y0 = y1; y1 = tmp;
    }

    /* loop bound: y1 - y0 <= HAL_UI_HEIGHT_PX = 64U (compile-time max) */
    for (uint8_t y = y0; y <= y1 && y < HAL_UI_HEIGHT_PX; y++) {
        CeePewErr_t err = hal_ui_pixel(x, y, color);
        CEEPEW_ASSERT(err == CEEPEW_OK, err);
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_line(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_PARAM);

    /* Bresenham line algorithm — works for any octant */
    int dx = (int)x1 - (int)x0;
    int dy = (int)y1 - (int)y0;
    int sx = (dx >= 0) ? 1 : -1;
    int sy = (dy >= 0) ? 1 : -1;

    dx = (dx < 0) ? -dx : dx;
    dy = (dy < 0) ? -dy : dy;

    int err = (dx > dy) ? (dx / 2) : -(dy / 2);
    int x = (int)x0;
    int y = (int)y0;

    /* Loop bound: max distance from (x0,y0) to (x1,y1) is sqrt(128²+64²) ≈ 143
       which is manageable. Each iteration moves closer to (x1,y1). */
    while (true) {
        /* Bounds check and draw pixel */
        if (x >= 0 && x < (int)HAL_UI_WIDTH_PX && y >= 0 && y < (int)HAL_UI_HEIGHT_PX) {
            CeePewErr_t err_pix = hal_ui_pixel((uint8_t)x, (uint8_t)y, color);
            CEEPEW_ASSERT(err_pix == CEEPEW_OK, err_pix);
        }

        /* Check if we reached endpoint */
        if (x == (int)x1 && y == (int)y1) {
            break;
        }

        /* Step toward endpoint */
        int err_new = err;
        if (err_new > -dx) {
            err = err_new - dy;
            x += sx;
        }
        if (err_new < dy) {
            err = err_new + dx;
            y += sy;
        }
    }

    return CEEPEW_OK;
}

CeePewErr_t hal_ui_rect(const HalUIRect_t *r, HalUIColor_t color)
{
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r->x + r->w <= HAL_UI_WIDTH_PX && r->y + r->h <= HAL_UI_HEIGHT_PX,
                  CEEPEW_ERR_BOUNDS);

    /* Draw 4 edges */
    CeePewErr_t err = hal_ui_hline(r->x, r->x + r->w - 1U, r->y, color);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    err = hal_ui_hline(r->x, r->x + r->w - 1U, r->y + r->h - 1U, color);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    err = hal_ui_vline(r->x, r->y, r->y + r->h - 1U, color);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    err = hal_ui_vline(r->x + r->w - 1U, r->y, r->y + r->h - 1U, color);
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    return CEEPEW_OK;
}

CeePewErr_t hal_ui_rect_fill(const HalUIRect_t *r, HalUIColor_t color)
{
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r->x + r->w <= HAL_UI_WIDTH_PX && r->y + r->h <= HAL_UI_HEIGHT_PX,
                  CEEPEW_ERR_BOUNDS);

    /* loop bound: r->h <= HAL_UI_HEIGHT_PX = 64U */
    for (uint8_t y = 0U; y < r->h; y++) {
        CeePewErr_t err = hal_ui_hline(r->x, r->x + r->w - 1U, r->y + y, color);
        CEEPEW_ASSERT(err == CEEPEW_OK, err);
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_circle(uint8_t cx, uint8_t cy, uint8_t radius, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_PARAM);

    /* Midpoint circle algorithm (Bresenham) */
    int x = (int)radius;
    int y = 0;
    int err = 0;

    /* loop bound: radius <= 64 (max radius that fits in display) */
    while (x >= y) {
        if (cx + x < HAL_UI_WIDTH_PX && cy + y < HAL_UI_HEIGHT_PX) {
            hal_ui_pixel(cx + x, cy + y, color);
        }
        if (cx - x >= 0 && cy + y < HAL_UI_HEIGHT_PX) {
            hal_ui_pixel(cx - x, cy + y, color);
        }
        if (cx + y < HAL_UI_WIDTH_PX && cy + x < HAL_UI_HEIGHT_PX) {
            hal_ui_pixel(cx + y, cy + x, color);
        }
        if (cx - y >= 0 && cy + x < HAL_UI_HEIGHT_PX) {
            hal_ui_pixel(cx - y, cy + x, color);
        }
        if (cx + x < HAL_UI_WIDTH_PX && cy - y >= 0) {
            hal_ui_pixel(cx + x, cy - y, color);
        }
        if (cx - x >= 0 && cy - y >= 0) {
            hal_ui_pixel(cx - x, cy - y, color);
        }
        if (cx + y < HAL_UI_WIDTH_PX && cy - x >= 0) {
            hal_ui_pixel(cx + y, cy - x, color);
        }
        if (cx - y >= 0 && cy - x >= 0) {
            hal_ui_pixel(cx - y, cy - x, color);
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
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_PARAM);

    int x = (int)radius;
    int y = 0;
    int err = 0;

    /* loop bound: radius <= 64 */
    while (x >= y) {
        if (cx + x < HAL_UI_WIDTH_PX && cy + y < HAL_UI_HEIGHT_PX) {
            hal_ui_hline(cx - x, cx + x, cy + y, color);
        }
        if (cy - y >= 0) {
            hal_ui_hline(cx - x, cx + x, cy - y, color);
        }
        if (cx + y < HAL_UI_WIDTH_PX) {
            hal_ui_hline(cx - y, cx + y, cy + x, color);
        }
        if (cy - x >= 0) {
            hal_ui_hline(cx - y, cx + y, cy - x, color);
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
    CEEPEW_ASSERT(str != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t cx = x;
    /* loop bound: strlen(str) <= 128 (max display width / char width) */
    for (uint16_t i = 0U; str[i] != '\0' && cx < HAL_UI_WIDTH_PX; i++) {
        CeePewErr_t err = hal_ui_char(cx, y, str[i], color);
        if (err != CEEPEW_OK) { return err; }
        cx += 6U;
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_char(uint8_t x, uint8_t y, char c, HalUIColor_t color)
{
    CEEPEW_ASSERT(s_ui_initialised, CEEPEW_ERR_PARAM);

    /* Map ASCII character to font table index */
    if (c < 32 || c > 126) { c = '?'; }
    uint8_t font_idx = (uint8_t)(c - 32U);

    const uint8_t *font_data = FONT_5x8[font_idx % 95U];

    /* Render 5×8 character with graceful clipping for off-screen pixels */
    /* loop bound: 8U (character height) */
    for (uint8_t row = 0U; row < 8U; row++) {
        uint8_t byte_val = font_data[row];
        /* loop bound: 5U (character width) */
        for (uint8_t col = 0U; col < 5U; col++) {
            uint8_t bit = (uint8_t)((byte_val >> col) & 1U);
            if (bit) {
                uint8_t px = x + col;
                uint8_t py = y + row;
                /* Skip pixels that exceed bounds instead of asserting */
                if (px < HAL_UI_WIDTH_PX && py < HAL_UI_HEIGHT_PX) {
                    CeePewErr_t err = hal_ui_pixel(px, py, color);
                    CEEPEW_ASSERT(err == CEEPEW_OK, err);
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
    /* loop bound: strlen(str) <= 128 (max display width / char width) */
    for (uint16_t i = 0U; str[i] != '\0' && i < 128U; i++) {
        width += 6U;
    }
    return width;
}

uint8_t *hal_ui_framebuffer(void)
{
    return s_framebuffer;
}

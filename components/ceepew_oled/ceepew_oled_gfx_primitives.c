/* components/ceepew_oled/ceepew_oled_gfx_primitives.c
 *
 * Hand-ported GFX primitives for the CEE-PEW OLED stack.
 *
 * Source provenance: drawing algorithms (Bresenham line, midpoint
 * circle, bitmap blit) are derived from the BSD-2-Clause Adafruit_GFX
 * library. They are pure-C ports that write directly into the
 * ceepew_oled framebuffer, with no vtable dispatch, no C++ runtime,
 * and no dynamic allocation.
 *
 * The font rendering (write() for a GFXfont) uses the column-major
 * "classic 5x7" pixel order that matches both Adafruit's glcdfont.c
 * and our existing s_font5x7[95][5] table. This is a deliberate
 * divergence from the row-major algorithm in Adafruit's full
 * GFXfont write(); the divergence is what lets us reuse the
 * existing 475-byte font data without conversion.
 */

#include "ceepew_oled_gfx_primitives.h"
#include "ceepew_oled.h"
#include "ceepew_oled_font_adapter.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Internal helpers ──────────────────────────────────────────────── */

/* Page-byte set/clear/invert, with bounds check. Mirrors the layout
 * assumed by hal_ui.c: 8 pages x 128 columns, page = y >> 3,
 * bit = 1 << (y & 7), index = page * 128 + x. */
static inline void fb_set_pixel(uint8_t *fb, int16_t x, int16_t y, HalUIColor_t color)
{
    if (x < 0 || x >= (int16_t)CEEPEW_OLED_WIDTH_PX)  { return; }
    if (y < 0 || y >= (int16_t)CEEPEW_OLED_HEIGHT_PX) { return; }
    const uint8_t  page = (uint8_t)((uint16_t)y >> 3U);
    const uint8_t  bit  = (uint8_t)(1U << (y & 0x07U));
    uint8_t       *byte = &fb[(uint16_t)page * (uint16_t)CEEPEW_OLED_WIDTH_PX + (uint16_t)x];
    switch (color) {
        case HAL_UI_WHITE:  *byte = (uint8_t)(*byte | bit);         break;
        case HAL_UI_BLACK:  *byte = (uint8_t)(*byte & (uint8_t)~bit); break;
        case HAL_UI_INVERT: *byte = (uint8_t)(*byte ^ bit);         break;
        default:            /* unreachable, kept for safety */       break;
    }
}

/* ── drawLine (Bresenham, page-byte direct write) ───────────────────── */

CeePewErr_t ceepew_oled_gfx_line(ceepew_oled_t *dev,
                                 int16_t x0, int16_t y0,
                                 int16_t x1, int16_t y1,
                                 HalUIColor_t color)
{
    CEEPEW_ASSERT(dev != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(color == HAL_UI_BLACK ||
                  color == HAL_UI_WHITE ||
                  color == HAL_UI_INVERT, CEEPEW_ERR_PARAM);
    uint8_t *fb = ceepew_oled_get_buffer(dev);
    CEEPEW_ASSERT(fb != NULL, CEEPEW_ERR_HW);

    int16_t dx = (x1 > x0) ? (int16_t)(x1 - x0) : (int16_t)(x0 - x1);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t dy = (y1 > y0) ? (int16_t)(y0 - y1) : (int16_t)(y1 - y0);
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = (int16_t)(dx + dy);

    /* Worst case: the line visits every panel pixel. Bound is
     * width + height - 1 = 191. Add one for safety. */
    const uint16_t max_iter = (uint16_t)(CEEPEW_OLED_WIDTH_PX + CEEPEW_OLED_HEIGHT_PX);
    for (uint16_t i = 0U; i <= max_iter; i++) {
        fb_set_pixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) { return CEEPEW_OK; }
        int16_t e2 = (int16_t)(2 * err);
        if (e2 >= dy) { err = (int16_t)(err + dy); x0 = (int16_t)(x0 + sx); }
        if (e2 <= dx) { err = (int16_t)(err + dx); y0 = (int16_t)(y0 + sy); }
    }
    return CEEPEW_OK;
}

/* ── drawRect / drawRectFill ───────────────────────────────────────── */

CeePewErr_t ceepew_oled_gfx_rect(ceepew_oled_t *dev,
                                 const HalUIRect_t *r,
                                 HalUIColor_t color)
{
    CEEPEW_ASSERT(dev != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r->w > 0U && r->h > 0U, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(r->x + r->w <= CEEPEW_OLED_WIDTH_PX,  CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(r->y + r->h <= CEEPEW_OLED_HEIGHT_PX, CEEPEW_ERR_BOUNDS);

    uint8_t *fb = ceepew_oled_get_buffer(dev);
    CEEPEW_ASSERT(fb != NULL, CEEPEW_ERR_HW);

    const uint8_t x0 = r->x;
    const uint8_t y0 = r->y;
    const uint8_t x1 = (uint8_t)(r->x + r->w - 1U);
    const uint8_t y1 = (uint8_t)(r->y + r->h - 1U);
    CeePewErr_t err = CEEPEW_OK;

    err = ceepew_oled_gfx_line(dev, (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y0, color);
    if (err != CEEPEW_OK) { return err; }
    err = ceepew_oled_gfx_line(dev, (int16_t)x0, (int16_t)y1, (int16_t)x1, (int16_t)y1, color);
    if (err != CEEPEW_OK) { return err; }
    err = ceepew_oled_gfx_line(dev, (int16_t)x0, (int16_t)y0, (int16_t)x0, (int16_t)y1, color);
    if (err != CEEPEW_OK) { return err; }
    err = ceepew_oled_gfx_line(dev, (int16_t)x1, (int16_t)y0, (int16_t)x1, (int16_t)y1, color);
    return err;
}

CeePewErr_t ceepew_oled_gfx_rect_fill(ceepew_oled_t *dev,
                                      const HalUIRect_t *r,
                                      HalUIColor_t color)
{
    CEEPEW_ASSERT(dev != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r->w > 0U && r->h > 0U, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(r->x + r->w <= CEEPEW_OLED_WIDTH_PX,  CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(r->y + r->h <= CEEPEW_OLED_HEIGHT_PX, CEEPEW_ERR_BOUNDS);

    uint8_t *fb = ceepew_oled_get_buffer(dev);
    CEEPEW_ASSERT(fb != NULL, CEEPEW_ERR_HW);

    for (uint8_t y = 0U; y < r->h; y++) {
        for (uint8_t x = 0U; x < r->w; x++) {
            fb_set_pixel(fb,
                         (int16_t)(r->x + x),
                         (int16_t)(r->y + y),
                         color);
        }
    }
    return CEEPEW_OK;
}

/* ── drawCircle / drawCircleFill (midpoint) ────────────────────────── */

static inline void circle_plot_8(uint8_t *fb, int16_t cx, int16_t cy,
                                 int16_t x, int16_t y, HalUIColor_t color)
{
    fb_set_pixel(fb, (int16_t)(cx + x), (int16_t)(cy + y), color);
    fb_set_pixel(fb, (int16_t)(cx - x), (int16_t)(cy + y), color);
    fb_set_pixel(fb, (int16_t)(cx + x), (int16_t)(cy - y), color);
    fb_set_pixel(fb, (int16_t)(cx - x), (int16_t)(cy - y), color);
    fb_set_pixel(fb, (int16_t)(cx + y), (int16_t)(cy + x), color);
    fb_set_pixel(fb, (int16_t)(cx - y), (int16_t)(cy + x), color);
    fb_set_pixel(fb, (int16_t)(cx + y), (int16_t)(cy - x), color);
    fb_set_pixel(fb, (int16_t)(cx - y), (int16_t)(cy - x), color);
}

CeePewErr_t ceepew_oled_gfx_circle(ceepew_oled_t *dev,
                                   uint8_t cx, uint8_t cy, uint8_t radius,
                                   HalUIColor_t color)
{
    CEEPEW_ASSERT(dev != NULL, CEEPEW_ERR_NULL_PTR);
    uint8_t *fb = ceepew_oled_get_buffer(dev);
    CEEPEW_ASSERT(fb != NULL, CEEPEW_ERR_HW);

    int16_t x = (int16_t)radius;
    int16_t y = 0;
    int16_t err = 0;
    while (x >= y) {
        circle_plot_8(fb, (int16_t)cx, (int16_t)cy, x, y, color);
        y = (int16_t)(y + 1);
        err = (int16_t)(err + (2 * y + 1));
        if ((2 * err - (2 * x - 1)) > 0) {
            x = (int16_t)(x - 1);
            err = (int16_t)(err + (1 - 2 * x));
        }
    }
    return CEEPEW_OK;
}

CeePewErr_t ceepew_oled_gfx_circle_fill(ceepew_oled_t *dev,
                                        uint8_t cx, uint8_t cy, uint8_t radius,
                                        HalUIColor_t color)
{
    CEEPEW_ASSERT(dev != NULL, CEEPEW_ERR_NULL_PTR);
    uint8_t *fb = ceepew_oled_get_buffer(dev);
    CEEPEW_ASSERT(fb != NULL, CEEPEW_ERR_HW);

    int16_t x = (int16_t)radius;
    int16_t y = 0;
    int16_t err = 0;
    while (x >= y) {
        for (int16_t xx = (int16_t)(cx - x); xx <= (int16_t)(cx + x); xx++) {
            fb_set_pixel(fb, xx, (int16_t)(cy + y), color);
            fb_set_pixel(fb, xx, (int16_t)(cy - y), color);
        }
        for (int16_t xx = (int16_t)(cx - y); xx <= (int16_t)(cx + y); xx++) {
            fb_set_pixel(fb, xx, (int16_t)(cy + x), color);
            fb_set_pixel(fb, xx, (int16_t)(cy - x), color);
        }
        y = (int16_t)(y + 1);
        err = (int16_t)(err + (2 * y + 1));
        if ((2 * err - (2 * x - 1)) > 0) {
            x = (int16_t)(x - 1);
            err = (int16_t)(err + (1 - 2 * x));
        }
    }
    return CEEPEW_OK;
}

/* ── drawBitmap (1-bit packed, MSB-first within each byte) ────────── */

CeePewErr_t ceepew_oled_gfx_bitmap(ceepew_oled_t *dev,
                                   int16_t x, int16_t y,
                                   const uint8_t *bitmap,
                                   int16_t w, int16_t h,
                                   HalUIColor_t color)
{
    CEEPEW_ASSERT(dev != NULL,  CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(bitmap != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(w > 0 && h > 0, CEEPEW_ERR_BOUNDS);
    /* Adafruit_GFX row-byte count: ceil(w / 8). The width is bounded
     * by the panel (128 px), and fb_set_pixel clips any out-of-range
     * coordinates, so we don't need a separate CEEPEW_ASSERT_BOUND. */
    const int16_t row_bytes = (int16_t)((w + 7) >> 3);

    uint8_t *fb = ceepew_oled_get_buffer(dev);
    CEEPEW_ASSERT(fb != NULL, CEEPEW_ERR_HW);

    for (int16_t row = 0; row < h; row++) {
        for (int16_t col = 0; col < w; col++) {
            const uint8_t byte = bitmap[(uint16_t)row * (uint16_t)row_bytes + (uint16_t)(col >> 3)];
            const uint8_t mask = (uint8_t)(0x80U >> (col & 0x07));
            if ((byte & mask) != 0U) {
                fb_set_pixel(fb, (int16_t)(x + col), (int16_t)(y + row), color);
            }
        }
    }
    return CEEPEW_OK;
}

/* ── Text (classic 5x7 column-major write()) ──────────────────────── */

static CeePewErr_t write_glyph(ceepew_oled_t *dev,
                               const ceepew_oled_GFXfont_t *font,
                               uint8_t c, int16_t x, int16_t y,
                               HalUIColor_t color)
{
    if (c < font->first || c > font->last) {
        return CEEPEW_OK;  /* not a renderable char in this font */
    }
    uint8_t *fb = ceepew_oled_get_buffer(dev);
    if (fb == NULL) { return CEEPEW_ERR_HW; }
    const ceepew_oled_GFXglyph_t *glyph = &font->glyph[c - font->first];

    /* Column-major classic 5x7 algorithm. s_font5x7[glyph_index][col]
     * stores one byte per column where bit 0 = top row. We iterate
     * column-by-column and shift the line byte down to expose each
     * row's bit. This is intentionally NOT the row-major Adafruit
     * GFXfont write() algorithm; the divergence is what lets us
     * reuse the existing 475-byte s_font5x7 data without
     * transposition. */
    const uint8_t *line_ptr = &font->bitmap[glyph->bitmapOffset];
    for (uint8_t col = 0U; col < glyph->width; col++) {
        uint8_t line = line_ptr[col];
        for (uint8_t row = 0U; row < glyph->height; row++) {
            if ((line & 0x01U) != 0U) {
                fb_set_pixel(fb, (int16_t)(x + glyph->xOffset + col),
                             (int16_t)(y + glyph->yOffset + row), color);
            }
            line = (uint8_t)(line >> 1);
        }
    }
    return CEEPEW_OK;
}

CeePewErr_t ceepew_oled_gfx_text(ceepew_oled_t *dev,
                                 uint8_t x, uint8_t y, const char *str,
                                 HalUIColor_t color)
{
    CEEPEW_ASSERT(dev != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(str != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(x < CEEPEW_OLED_WIDTH_PX,  CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(y < CEEPEW_OLED_HEIGHT_PX, CEEPEW_ERR_BOUNDS);

    const ceepew_oled_GFXfont_t *font = ceepew_oled_get_default_font();
    CEEPEW_ASSERT(font != NULL, CEEPEW_ERR_HW);

    int16_t cursor = (int16_t)x;
    for (uint16_t i = 0U; str[i] != '\0'; i++) {
        const char ch = str[i];
        if (ch == '\n') {
            y = (uint8_t)(y + font->yAdvance);
            cursor = (int16_t)x;
            continue;
        }
        CeePewErr_t err = write_glyph(dev, font, (uint8_t)ch, cursor, (int16_t)y, color);
        if (err != CEEPEW_OK) { return err; }
        /* Advance by xAdvance. For the 5x7 default font this is 6 px
         * (5 glyph + 1 gap). */
        const ceepew_oled_GFXglyph_t *glyph = NULL;
        if ((uint8_t)ch >= font->first && (uint8_t)ch <= font->last) {
            glyph = &font->glyph[(uint8_t)ch - font->first];
        }
        if (glyph != NULL) {
            cursor = (int16_t)(cursor + glyph->xAdvance);
        } else {
            cursor = (int16_t)(cursor + 6);
        }
        if (cursor >= (int16_t)CEEPEW_OLED_WIDTH_PX) { break; }
    }
    return CEEPEW_OK;
}

uint16_t ceepew_oled_gfx_text_width(const char *str)
{
    if (str == NULL) { return 0U; }
    const ceepew_oled_GFXfont_t *font = ceepew_oled_get_default_font();
    if (font == NULL) { return 0U; }
    uint16_t width = 0U;
    for (uint16_t i = 0U; str[i] != '\0' && i < 256U; i++) {
        const uint8_t ch = (uint8_t)str[i];
        if (ch >= font->first && ch <= font->last) {
            width = (uint16_t)(width + font->glyph[ch - font->first].xAdvance);
        } else {
            width = (uint16_t)(width + 6U);
        }
    }
    return width;
}

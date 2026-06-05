/* components/ceepew_oled/ceepew_oled_gfx_primitives.h
 *
 * Hand-ported GFX-style drawing primitives for the CEE-PEW OLED stack.
 *
 * The functions here are pure framebuffer writers: they touch
 * ceepew_oled's 1024-byte framebuffer and never push to the panel.
 * hal_ui_flush() is responsible for the I2C push.
 *
 * Design choices:
 *   - Pure C, no C++ runtime. The function-pointer-based vtable of
 *     Adafruit_GFX is replaced by a flat C API; the call cost is one
 *     branch per primitive (negligible).
 *   - drawLine writes directly into the page-byte (no per-pixel
 *     hal_ui_pixel round-trip), so it is faster than a vtable dispatch
 *     on a Bresenham step would be.
 *   - drawBitmap interprets the input as 1-bit packed, MSB-first,
 *     matching Adafruit_GFX's bitblt convention.
 *   - Text rendering uses the GFXfont-aware write() in
 *     ceepew_oled_font_adapter.c, which exposes Adafruit_GFX's GFXfont
 *     format but consumes the existing s_font5x7 byte data.
 *
 * Functions derived from Adafruit_GFX.cpp (BSD-2-Clause) — see the
 * component LICENSE file.
 */

#ifndef CEEPEW_OLED_GFX_PRIMITIVES_H
#define CEEPEW_OLED_GFX_PRIMITIVES_H

#include <stdint.h>
#include "hal_ui_types.h"
#include "ceepew_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration; the full struct definition lives in ceepew_oled.h.
 * This keeps the GFX header free of i2c_master / gpio dependencies, so
 * callers that only draw (no I2C) can include just this file. */
typedef struct ceepew_oled_t ceepew_oled_t;

/* Bresenham line, page-byte direct write. Endpoints are inclusive and
 * do NOT need to be in any order. Bounds-checked against HAL_UI_WIDTH_PX
 * and HAL_UI_HEIGHT_PX; out-of-range endpoints are clamped, not rejected.
 * The `dev` handle identifies the panel whose framebuffer to write to. */
CeePewErr_t ceepew_oled_gfx_line(ceepew_oled_t *dev,
                                 int16_t x0, int16_t y0,
                                 int16_t x1, int16_t y1,
                                 HalUIColor_t color);

/* Outline rectangle. r->w must be >= 1 and r->h must be >= 1. */
CeePewErr_t ceepew_oled_gfx_rect(ceepew_oled_t *dev,
                                 const HalUIRect_t *r,
                                 HalUIColor_t color);

/* Filled rectangle. r->w must be >= 1 and r->h must be >= 1. */
CeePewErr_t ceepew_oled_gfx_rect_fill(ceepew_oled_t *dev,
                                      const HalUIRect_t *r,
                                      HalUIColor_t color);

/* Midpoint-circle outline. Radius clamped to <= 63. */
CeePewErr_t ceepew_oled_gfx_circle(ceepew_oled_t *dev,
                                   uint8_t cx, uint8_t cy, uint8_t radius,
                                   HalUIColor_t color);

/* Midpoint-circle filled. Radius clamped to <= 63. */
CeePewErr_t ceepew_oled_gfx_circle_fill(ceepew_oled_t *dev,
                                        uint8_t cx, uint8_t cy,
                                        uint8_t radius,
                                        HalUIColor_t color);

/* 1-bit packed bitmap blit, MSB-first (Adafruit_GFX convention). Each
 * row is ceil(w / 8) bytes; bit 7 of byte 0 is the leftmost pixel of
 * each row. Pixels outside the panel are clipped, not rejected. */
CeePewErr_t ceepew_oled_gfx_bitmap(ceepew_oled_t *dev,
                                   int16_t x, int16_t y,
                                   const uint8_t *bitmap,
                                   int16_t w, int16_t h,
                                   HalUIColor_t color);

/* Text rendering via the GFXfont-aware write(). Each glyph is 5x7 with
 * 1px column gap = 6px advance, matching the existing s_font5x7 table. */
CeePewErr_t ceepew_oled_gfx_text(ceepew_oled_t *dev,
                                 uint8_t x, uint8_t y, const char *str,
                                 HalUIColor_t color);

/* Measure text width in pixels: 6 px per printable ASCII character. */
uint16_t    ceepew_oled_gfx_text_width(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* CEEPEW_OLED_GFX_PRIMITIVES_H */

/* components/ceepew_oled/ceepew_oled_font_adapter.h
 *
 * GFXfont-format adapter for the existing s_font5x7[95][5] data.
 *
 * Adafruit_GFX (BSD-2-Clause) defines a two-struct pair for variable-size
 * fonts: GFXglyph describes one glyph (offset, width, height, advance,
 * x/y offset) and GFXfont describes the whole font (bitmap pointer,
 * glyph pointer, first/last char, line height). This file ships a
 * GFXfont that points at the existing s_font5x7 byte data — the bitmap
 * layout is byte-for-byte identical to Adafruit's glcdfont.c, so no
 * new font bytes are introduced.
 *
 * The pointer to s_font5x7 is resolved at link time; the symbol lives
 * in components/ceepew_hal/ui_manager.c and is referenced by extern.
 * If the link fails because the symbol is missing, define
 * CEEPEW_OLED_FONT_FALLBACK_LOCAL=1 in font_adapter.c to use a local
 * copy (see the fallback #ifdef at the top of font_adapter.c).
 */

#ifndef CEEPEW_OLED_FONT_ADAPTER_H
#define CEEPEW_OLED_FONT_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Single glyph descriptor. Field layout matches Adafruit_GFX exactly. */
typedef struct {
    uint16_t bitmapOffset;     /* offset into the GFXfont bitmap array   */
    uint8_t  width;            /* bitmap dimensions in pixels (cols)     */
    uint8_t  height;           /* bitmap dimensions in pixels (rows)     */
    uint8_t  xAdvance;         /* distance to advance cursor (pixels)    */
    int8_t   xOffset;          /* x offset from cursor position          */
    int8_t   yOffset;          /* y offset from cursor position          */
} ceepew_oled_GFXglyph_t;

/* Whole-font descriptor. */
typedef struct {
    const uint8_t             *bitmap;   /* glyph bitmap, concatenated  */
    const ceepew_oled_GFXglyph_t *glyph; /* per-glyph descriptors      */
    uint16_t                  first;     /* first ASCII char in font    */
    uint16_t                  last;      /* last ASCII char in font     */
    uint8_t                   yAdvance;  /* newline distance (pixels)   */
} ceepew_oled_GFXfont_t;

/* Return the bundled 5x7 monospace font. Always non-NULL; the pointer
 * is to a static const in flash, so callers never own the storage. */
const ceepew_oled_GFXfont_t *ceepew_oled_get_default_font(void);

#ifdef __cplusplus
}
#endif

#endif /* CEEPEW_OLED_FONT_ADAPTER_H */

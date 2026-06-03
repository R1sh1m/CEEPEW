/* components/ceepew_hal/hal_ui.h
 *
 * Core UI rendering API: abstraction over SSD1306 OLED display.
 * Provides primitive drawing (pixels, lines, rects, circles),
 * font rendering, and a simple scene graph for UI composition.
 *
 * The display driver underneath is the vendored ssd1306 component
 * (nopnop2002/esp-idf-ssd1306, MIT, see components/ssd1306/LICENSE).
 * All hal_ui_* drawing primitives write into that driver's internal
 * page buffer; hal_ui_flush() pushes all 8 pages over I2C in
 * page-addressing mode (no malloc, no async queue).
 */

#ifndef HAL_UI_H
#define HAL_UI_H

#include <stdint.h>
#include <stdbool.h>
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ssd1306.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Display dimensions (SSD1306 128×64) */
#define HAL_UI_WIDTH_PX   128U
#define HAL_UI_HEIGHT_PX  64U

/* Color modes (1-bit display: on/off) */
typedef enum {
    HAL_UI_BLACK = 0U,
    HAL_UI_WHITE = 1U,
    HAL_UI_INVERT = 2U
} HalUIColor_t;

/* Rectangular region: (x, y) origin, (w, h) size */
typedef struct {
    uint8_t  x;
    uint8_t  y;
    uint8_t  w;
    uint8_t  h;
} HalUIRect_t;

/* Initialize UI subsystem and bring up the OLED panel. Replaces the
 * prior hal_oled_init(); the new flow performs I2C bus recovery, tries
 * the configured (pin × speed × address) matrix, then falls back to a
 * full GPIO-pair scan, all within hal_ui_init().
 *
 * Returns CEEPEW_OK on success, CEEPEW_ERR_HW if no SSD1306 was found. */
CeePewErr_t hal_ui_init(void);

/* Clear entire display (fill with black) */
CeePewErr_t hal_ui_clear(void);

/* Flush framebuffer to display (refresh the OLED). Retries once with
 * the SH1106 +2 column offset if the SSD1306 flush fails. If nothing
 * has been drawn since the last successful flush, this is a no-op
 * (no I2C traffic). */
CeePewErr_t hal_ui_flush(void);

/* Set a single pixel at (x, y) */
CeePewErr_t hal_ui_pixel(uint8_t x, uint8_t y, HalUIColor_t color);

/* Draw horizontal line from (x0, y) to (x1, y) */
CeePewErr_t hal_ui_hline(uint8_t x0, uint8_t x1, uint8_t y, HalUIColor_t color);

/* Draw vertical line from (x, y0) to (x, y1) */
CeePewErr_t hal_ui_vline(uint8_t x, uint8_t y0, uint8_t y1, HalUIColor_t color);

/* Draw line from (x0, y0) to (x1, y1) using Bresenham algorithm */
CeePewErr_t hal_ui_line(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, HalUIColor_t color);

/* Draw rectangle outline at region */
CeePewErr_t hal_ui_rect(const HalUIRect_t *r, HalUIColor_t color);

/* Draw filled rectangle at region */
CeePewErr_t hal_ui_rect_fill(const HalUIRect_t *r, HalUIColor_t color);

/* Draw circle centered at (cx, cy) with radius r (Bresenham algorithm) */
CeePewErr_t hal_ui_circle(uint8_t cx, uint8_t cy, uint8_t radius, HalUIColor_t color);

/* Draw filled circle */
CeePewErr_t hal_ui_circle_fill(uint8_t cx, uint8_t cy, uint8_t radius, HalUIColor_t color);

/* Draw text string at (x, y) in 5×7 monospace font (character width=6) */
CeePewErr_t hal_ui_text(uint8_t x, uint8_t y, const char *str, HalUIColor_t color);

/* Draw single character at (x, y) */
CeePewErr_t hal_ui_char(uint8_t x, uint8_t y, char c, HalUIColor_t color);

/* Measure text width in pixels (6 pixels per character) */
uint16_t hal_ui_text_width(const char *str);

/* Fit text into pixel width, safe output buffer. If truncated, appends '...' when space allows. */
CeePewErr_t hal_ui_fit_text(const char *src, uint8_t max_px_width, char *out, uint8_t out_size);

/* Draw text by inverting pixels over existing background; useful for black-on-white text. */
CeePewErr_t hal_ui_text_invert(uint8_t x, uint8_t y, const char *str);

/* Return a pointer to the underlying SSD1306_t device (vendored driver).
 * Returns NULL if hal_ui_init has not succeeded. Used by the boot
 * splash in main.c to access charge-pump control and raw I2C commands.
 *
 * Note: callers MUST NOT call ssd1306_init() on the returned device,
 * and MUST NOT modify _i2c_bus_handle / _i2c_dev_handle. */
SSD1306_t *hal_ui_get_dev(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_UI_H */

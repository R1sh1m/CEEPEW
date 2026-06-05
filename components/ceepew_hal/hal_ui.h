/* components/ceepew_hal/hal_ui.h
 *
 * Core UI rendering API: abstraction over SSD1306 OLED display.
 * Provides primitive drawing (pixels, lines, rects, circles),
 * font rendering, and a simple scene graph for UI composition.
 *
 * The display driver underneath is the in-house ceepew_oled component
 * (see components/ceepew_oled/, MIT). All hal_ui_* drawing primitives
 * write into that driver's framebuffer; hal_ui_flush() pushes the
 * framebuffer to the panel over I2C using the driver's
 * ceepew_oled_display() / ceepew_oled_display_sh1106() entry points.
 */

#ifndef HAL_UI_H
#define HAL_UI_H

#include <stdint.h>
#include <stdbool.h>
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "hal_ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Display dimensions (SSD1306 128×64) */
#define HAL_UI_WIDTH_PX   128U
#define HAL_UI_HEIGHT_PX  64U

/* HalUIColor_t and HalUIRect_t live in hal_ui_types.h so they can be
 * shared with components/ceepew_oled/ceepew_oled_gfx_primitives.h without
 * a circular include. */



/* Initialize UI subsystem and bring up the OLED panel. Performs I2C
 * bus recovery, tries the configured (pin × speed × address) matrix,
 * then falls back to a full GPIO-pair scan, all within hal_ui_init().
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

#ifdef __cplusplus
}
#endif

#endif /* HAL_UI_H */

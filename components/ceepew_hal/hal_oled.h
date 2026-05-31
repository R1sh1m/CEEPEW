/* components/ceepew_hal/hal_oled.h */
#ifndef CEEPEW_HAL_OLED_H
#define CEEPEW_HAL_OLED_H

#include <stdbool.h>
#include <stdint.h>
#include "ceepew_assert.h"

CeePewErr_t hal_oled_init(void);
CeePewErr_t hal_oled_clear(void);
CeePewErr_t hal_oled_flush(void);
CeePewErr_t hal_oled_blit(const uint8_t *framebuffer, uint32_t len);
CeePewErr_t hal_oled_selftest_flash(uint32_t duration_ms);
CeePewErr_t hal_oled_set_charge_pump(bool enabled);
CeePewErr_t hal_oled_draw_pixel(uint8_t x, uint8_t y, bool on);
CeePewErr_t hal_oled_draw_line(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, bool on);
CeePewErr_t hal_oled_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on);
CeePewErr_t hal_oled_draw_char(uint8_t x, uint8_t y, char ch);
CeePewErr_t hal_oled_draw_text(uint8_t x, uint8_t y, const char *text);

#endif /* CEEPEW_HAL_OLED_H */

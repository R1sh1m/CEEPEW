/* components/ceepew_hal/hal_ui_helpers.c */

#include "hal_ui.h"
#include "ceepew_assert.h"
#include <string.h>
#include <stdio.h>

/* Design note: Helper utilities for text layout and inversion are split into
   a separate translation unit to keep hal_ui.c small and to avoid touching
   the SSD1306-specific drawing primitives. These helpers are fully
   deterministic, bounds-checked, and avoid dynamic allocation. */

CeePewErr_t hal_ui_fit_text(const char *src, uint8_t max_px_width, char *out, uint8_t out_size)
{
    CEEPEW_ASSERT(src != NULL && out != NULL && out_size > 0U, CEEPEW_ERR_NULL_PTR);

    /* Each monospace character occupies 6 pixels (5 glyph + 1 gap) */
    uint8_t max_chars = (uint8_t)(max_px_width / 6U);
    if (max_chars == 0U) {
        out[0] = '\0';
        return CEEPEW_OK;
    }

    size_t src_len = strlen(src);
    /* If it fits and buffer is large enough, copy directly */
    if (src_len <= max_chars && src_len < (size_t)out_size) {
        memcpy(out, src, src_len);
        out[src_len] = '\0';
        return CEEPEW_OK;
    }

    /* Need to truncate. Prefer appending "..." if space allows. */
    if (max_chars <= 3U) {
        /* Not enough room for ellipses; just copy what fits */
        uint8_t copy = (uint8_t)((out_size - 1U < max_chars) ? (out_size - 1U) : max_chars);
        if (copy > 0U) {
            memcpy(out, src, copy);
        }
        out[copy] = '\0';
        return CEEPEW_OK;
    }

    uint8_t content_chars = (uint8_t)(max_chars - 3U); /* reserve 3 for '...' */
    /* Ensure we don't overflow output buffer */
    uint8_t copy = (uint8_t)((out_size - 1U < (uint8_t)(content_chars + 3U)) ? (out_size - 1U - 3U) : content_chars);
    if (copy > 0U) {
        memcpy(out, src, copy);
    }
    size_t pos = copy;
    if (pos < out_size - 1U) { out[pos++] = '.'; }
    if (pos < out_size - 1U) { out[pos++] = '.'; }
    if (pos < out_size - 1U) { out[pos++] = '.'; }
    out[pos] = '\0';
    return CEEPEW_OK;
}

CeePewErr_t hal_ui_text_invert(uint8_t x, uint8_t y, const char *str)
{
    CEEPEW_ASSERT(str != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t cx = x;
    for (size_t i = 0U; str[i] != '\0'; i++) {
        /* Keep characters on-screen; hal_ui_char will itself bound-check */
        CeePewErr_t err = hal_ui_char(cx, y, (char)str[i], HAL_UI_INVERT);
        if (err != CEEPEW_OK) { return err; }
        cx = (uint8_t)(cx + 6U);
        if (cx >= HAL_UI_WIDTH_PX) { break; }
    }
    return CEEPEW_OK;
}

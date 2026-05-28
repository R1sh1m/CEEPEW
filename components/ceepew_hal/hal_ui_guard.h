/* components/ceepew_hal/hal_ui_guard.h */

#ifndef CEEPEW_HAL_UI_GUARD_H
#define CEEPEW_HAL_UI_GUARD_H

#include <stddef.h>

#include "layout.h"
#include "ceepew_assert.h"

/* Design note: The guard is intentionally thin. It delegates the actual
   geometry model to layout.c and only provides a small set of checked helpers
   that the render layer can call before touching the framebuffer. */

static inline CeePewErr_t ui_guard_check_point(UIState_t state, uint8_t x, uint8_t y){
    return layout_point_allowed(state, x, y) ? CEEPEW_OK : CEEPEW_ERR_BOUNDS;
}

static inline CeePewErr_t ui_guard_check_rect(UIState_t state, const HalUIRect_t *rect){
    return layout_rect_allowed(state, rect) ? CEEPEW_OK : CEEPEW_ERR_BOUNDS;
}

static inline CeePewErr_t ui_guard_check_text_box(UIState_t state, uint8_t x, uint8_t y, const char *text){
    CEEPEW_ASSERT(text != NULL, CEEPEW_ERR_NULL_PTR);

    uint16_t width_px = hal_ui_text_width(text);
    if (width_px == 0U) {
        return CEEPEW_OK;
    }

    if (width_px > 255U) {
        width_px = 255U;
    }

    HalUIRect_t box = {
        .x = x,
        .y = y,
        .w = (uint8_t)width_px,
        .h = 8U
    };

    return ui_guard_check_rect(state, &box);
}

#endif /* CEEPEW_HAL_UI_GUARD_H */

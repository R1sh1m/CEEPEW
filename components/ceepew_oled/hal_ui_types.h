/* components/ceepew_oled/hal_ui_types.h
 *
 * Shared leaf types for the OLED / UI rendering stack.
 *
 * Lives in the OLED component (lowest layer of the drawing pipeline) and
 * is included by both components/ceepew_hal/hal_ui.h and the new
 * components/ceepew_oled/ceepew_oled_gfx_primitives.h. This avoids the
 * circular include that would arise if the types lived in hal_ui.h while
 * the GFX primitives in ceepew_oled/ needed to see them.
 *
 * CeePewErr_t also lives here (was previously in main/ceepew_assert.h)
 * so that the GFX header can use it without depending on main. The
 * ceepew_assert.h header #includes this file and re-uses the type, so
 * all existing call sites continue to work unchanged.
 *
 * If you are tempted to add a new field here: it must be a leaf POD type
 * with no other project-include dependencies. This header is on the
 * critical path of every UI header.
 */

#ifndef CEEPEW_HAL_UI_TYPES_H
#define CEEPEW_HAL_UI_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Project-wide error code enum. The numeric values are stable and
 * part of the public ABI; do not renumber. Was previously defined in
 * main/ceepew_assert.h. */
typedef enum {
    CEEPEW_OK = 0,
    CEEPEW_ERR_NULL_PTR = 1,
    CEEPEW_ERR_BOUNDS = 2,
    CEEPEW_ERR_PARAM = 3,
    CEEPEW_ERR_NONCE_EXHAUSTED = 4,
    CEEPEW_ERR_CRYPTO = 5,
    CEEPEW_ERR_TRANSPORT = 6,
    CEEPEW_ERR_FEC = 7,
    CEEPEW_ERR_PINS = 8,
    CEEPEW_ERR_ALLOC = 9,
    CEEPEW_ERR_INTERNAL = 10,
    CEEPEW_ERR_TIMEOUT = 11,
    CEEPEW_ERR_OVERFLOW = 12,
    CEEPEW_ERR_UNSUPPORTED = 13,
    CEEPEW_ERR_BUSY = 14,
    CEEPEW_ERR_HW = 15,
    CEEPEW_ERR_NOENT = 16,
    CEEPEW_ERR_REPLAY = 17,
    CEEPEW_ERR_SIG_FAIL = 18,
    CEEPEW_ERR_MAX_RETRIES = 19,
    CEEPEW_ERR_AUTH_FAIL = 20,
    CEEPEW_ERR_FEC_UNCORRECT = 21,
    CEEPEW_ERR_NONCE_NEARLY_EXHAUSTED = 22,
    CEEPEW_ERR_NEED_TX = 23
} CeePewErr_t;

/* Color modes for the 1-bit OLED panel. HAL_UI_INVERT XORs the underlying
 * framebuffer bit, which is used to draw black-on-white text by inverting
 * over a filled background. */
typedef enum {
    HAL_UI_BLACK  = 0U,
    HAL_UI_WHITE  = 1U,
    HAL_UI_INVERT = 2U
} HalUIColor_t;

/* Rectangular region: (x, y) origin, (w, h) size in pixels. */
typedef struct {
    uint8_t  x;
    uint8_t  y;
    uint8_t  w;
    uint8_t  h;
} HalUIRect_t;

#ifdef __cplusplus
}
#endif

#endif /* CEEPEW_HAL_UI_TYPES_H */

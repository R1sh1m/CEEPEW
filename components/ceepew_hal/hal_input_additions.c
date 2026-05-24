/* components/ceepew_hal/hal_input_additions.c
 *
 * Helper additions for input: provide normalized 8-bit pot value and
 * simple accessor to button and diag states. Keep assertions to two
 * per function to satisfy CEEPEW coding guidelines.
 */

#include "hal_input.h"
#include "ceepew_assert.h"

CeePewErr_t input_get_normalized(const InputCtx_t *ctx, uint8_t *out_pot, bool *out_button, bool *out_diag){
    /* Two assertions as per coding standard */
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out_pot != NULL, CEEPEW_ERR_NULL_PTR);

    /* Map 16-bit cursor_pos (0..65535) to 8-bit range (0..255) */
    uint32_t pos = (uint32_t)ctx->cursor_pos;
    uint8_t pot8 = (uint8_t)((pos * 255U) / 65535U);
    *out_pot = pot8;

    if (out_button != NULL){
        *out_button = ctx->button_stable_state;
    }
    if (out_diag != NULL){
        *out_diag = ctx->diag_switch_active;
    }
    return CEEPEW_OK;
}

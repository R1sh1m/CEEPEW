/* components/ceepew_hal/hal_input_additions.c
 *
 * Helper additions for input: provide normalized 8-bit pot value and
 * simple accessor to button and diag states. Keep assertions to two
 * per function to satisfy CEEPEW coding guidelines.
 */

#include "hal_input.h"
#include "ceepew_assert.h"
#include "hal_adc.h"
#include "ceepew_config.h"

CeePewErr_t input_get_normalized(const InputCtx_t *ctx, uint8_t *out_pot, bool *out_button, bool *out_diag){
    /* Two assertions as per coding standard */
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out_pot != NULL, CEEPEW_ERR_NULL_PTR);

    /* Map 16-bit cursor_pos (0..65535) to 8-bit range (0..255).
       Use shift to avoid division rounding bias and retain linearity. */
    uint32_t pos = (uint32_t)ctx->cursor_pos;
    uint8_t pot8 = (uint8_t)(pos >> 8U); /* 65536 >> 8 == 256 discrete values */
    *out_pot = pot8;

    if (out_button != NULL){
        *out_button = ctx->button_stable_state;
    }
    if (out_diag != NULL){
        *out_diag = ctx->diag_switch_active;
    }
    return CEEPEW_OK;
}

CeePewErr_t input_get_adc_snapshot(uint16_t *raw_out, uint16_t *smoothed_out)
{
    CEEPEW_ASSERT(raw_out != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(smoothed_out != NULL, CEEPEW_ERR_NULL_PTR);

    /* Read a raw ADC sample using hal_adc API (handles oversampling) */
    uint16_t raw = 0U;
    CeePewErr_t err = hal_adc_read_raw(&raw);
    if (err != CEEPEW_OK) { return err; }

    *raw_out = raw;

    /* Simple edge-compensated smoothed estimate: clamp near rails */
    if (raw < CEEPEW_POT_DEADZONE) {
        *smoothed_out = 0U;
    } else if (raw > (CEEPEW_ADC_MAX_RAW - CEEPEW_POT_DEADZONE)) {
        *smoothed_out = CEEPEW_ADC_MAX_RAW;
    } else {
        *smoothed_out = raw;
    }
    return CEEPEW_OK;
}

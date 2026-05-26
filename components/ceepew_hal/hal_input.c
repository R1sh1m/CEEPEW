/* components/hal/hal_input.c */
#include "hal_input.h"
#include "hal_adc.h"
#include "ceepew_config.h"
#include "hal_pins.h"
#include "driver/gpio.h"

/* Defaults from spec if not configured elsewhere */
#ifndef CEEPEW_POT_EMA_ALPHA_NUM
/* Make pot feel snappier: alpha = 25/100 (0.25) */
#define CEEPEW_POT_EMA_ALPHA_NUM 25U
#endif
#ifndef CEEPEW_POT_EMA_ALPHA_DEN
#define CEEPEW_POT_EMA_ALPHA_DEN 100U
#endif
#ifndef CEEPEW_POT_DEADZONE
/* Reduce deadzone so full travel maps to characters */
#define CEEPEW_POT_DEADZONE 60U
#endif
#ifndef CEEPEW_POT_EDGE_HYSTERESIS
/* Tighter hysteresis for edge stability */
#define CEEPEW_POT_EDGE_HYSTERESIS 16U
#endif

static uint16_t ema_update(uint16_t prev_ema, uint16_t new_sample)
{
    uint32_t result = ((uint32_t)CEEPEW_POT_EMA_ALPHA_NUM * (uint32_t)new_sample) +
                      (((uint32_t)CEEPEW_POT_EMA_ALPHA_DEN - (uint32_t)CEEPEW_POT_EMA_ALPHA_NUM) *
                       (uint32_t)prev_ema);
    return (uint16_t)(result / (uint32_t)CEEPEW_POT_EMA_ALPHA_DEN);
}

static uint16_t apply_edge_compensation(uint16_t smoothed)
{
    if (smoothed < CEEPEW_POT_DEADZONE) {
        return 0U;
    }
    if (smoothed > (CEEPEW_ADC_MAX_RAW - CEEPEW_POT_DEADZONE)) {
        return (uint16_t)CEEPEW_ADC_MAX_RAW;
    }
    return smoothed;
}

CeePewErr_t input_init(InputCtx_t *ctx) {
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(!ctx->initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(CEEPEW_POT_EMA_ALPHA_NUM < CEEPEW_POT_EMA_ALPHA_DEN, CEEPEW_ERR_PARAM);
    /* Initialize public state. */
    ctx->raw_adc = 0U;
    ctx->smoothed_adc = 0U;
    ctx->cursor_pos = 0U;
    ctx->at_left_edge = false;
    ctx->at_right_edge = false;
    ctx->edge_stable_since = 0U;
    ctx->last_click_ms = 0U;
    ctx->click_pending = false;
    ctx->diag_switch_active = false;
    ctx->button_raw_state = true;
    ctx->button_stable_state = true; /* true means pressed; set from GPIO below */
    ctx->button_raw_change_ms = 0U;
    ctx->button_last_change_ms = 0U;
    /* Seed smoothed value from first ADC sample. */
    uint16_t raw = 0U;
    CeePewErr_t err = hal_adc_read_raw(&raw);
    if (err != CEEPEW_OK) { return err; }
    ctx->raw_adc = raw;
    ctx->smoothed_adc = apply_edge_compensation(raw);
    ctx->at_left_edge = (ctx->smoothed_adc <= CEEPEW_POT_DEADZONE);
    ctx->at_right_edge = (ctx->smoothed_adc >= (CEEPEW_ADC_MAX_RAW - CEEPEW_POT_DEADZONE));
    ctx->cursor_pos = (uint16_t)(((uint32_t)ctx->smoothed_adc * 65535U) / (uint32_t)CEEPEW_ADC_MAX_RAW);
    /* Initialize button states from current hardware level. */
    int raw_button_level = gpio_get_level(CEEPEW_PIN_BUTTON);
    bool pressed_now = (raw_button_level == CEEPEW_BUTTON_ACTIVE_LEVEL);
    ctx->button_raw_state = pressed_now;
    ctx->button_stable_state = pressed_now;
    /* Initialize DIAG switch state from current hardware level. */
    int diag_level = gpio_get_level(CEEPEW_PIN_DIAG_SWITCH);
    ctx->diag_switch_active = (diag_level == CEEPEW_DIAG_SWITCH_ACTIVE);
    ctx->initialised = true;
    return CEEPEW_OK;
}

CeePewErr_t input_update(InputCtx_t *ctx, uint32_t now_ms){
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(ctx->initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(CEEPEW_POT_EMA_ALPHA_DEN > CEEPEW_POT_EMA_ALPHA_NUM, CEEPEW_ERR_PARAM);
    uint16_t raw = 0U;
    CeePewErr_t err = hal_adc_read_raw(&raw);
    if (err != CEEPEW_OK){ return err;}
    ctx->raw_adc = raw;
    ctx->smoothed_adc = apply_edge_compensation(ema_update(ctx->smoothed_adc, raw));
    /* Cursor maps to full 16-bit UI range. */
    ctx->cursor_pos = (uint16_t)(((uint32_t)ctx->smoothed_adc * 65535U) / (uint32_t)CEEPEW_ADC_MAX_RAW);
    /* Edge detection with hysteresis to suppress oscillation around thresholds. */
    uint16_t left_on = (uint16_t)CEEPEW_POT_DEADZONE;
    uint16_t left_off = (uint16_t)(CEEPEW_POT_DEADZONE + CEEPEW_POT_EDGE_HYSTERESIS);
    if (left_off > CEEPEW_ADC_MAX_RAW){ left_off = (uint16_t)CEEPEW_ADC_MAX_RAW;}
    uint16_t right_on = (uint16_t)(CEEPEW_ADC_MAX_RAW - CEEPEW_POT_DEADZONE);
    uint16_t right_off;
    if ((CEEPEW_POT_DEADZONE + CEEPEW_POT_EDGE_HYSTERESIS) > CEEPEW_ADC_MAX_RAW){ right_off = 0U;}
    else{
        right_off = (uint16_t)(CEEPEW_ADC_MAX_RAW - CEEPEW_POT_DEADZONE - CEEPEW_POT_EDGE_HYSTERESIS);
    }
    bool left_now = ctx->at_left_edge ? (ctx->smoothed_adc <= left_off) : (ctx->smoothed_adc <= left_on);
    bool right_now = ctx->at_right_edge ? (ctx->smoothed_adc >= right_off) : (ctx->smoothed_adc >= right_on);
    if (left_now != ctx->at_left_edge || right_now != ctx->at_right_edge){ ctx->edge_stable_since = now_ms;}
    ctx->at_left_edge = left_now;
    ctx->at_right_edge = right_now;
    /* Read DIAG switch (polled, active LOW) */
    int diag_level = gpio_get_level(CEEPEW_PIN_DIAG_SWITCH);
    ctx->diag_switch_active = (diag_level == CEEPEW_DIAG_SWITCH_ACTIVE);
    /* Debounce path: track raw transitions, accept only when stable for debounce window. */
    int raw_button = gpio_get_level(CEEPEW_PIN_BUTTON);
    bool raw_pressed = (raw_button == CEEPEW_BUTTON_ACTIVE_LEVEL);
    if (raw_pressed != ctx->button_raw_state){
        ctx->button_raw_state = raw_pressed;
        ctx->button_raw_change_ms = now_ms;
    }
    uint32_t stable_for_ms = now_ms - ctx->button_raw_change_ms;
    if (ctx->button_stable_state != ctx->button_raw_state && stable_for_ms >= CEEPEW_BUTTON_DEBOUNCE_MS){
        ctx->button_stable_state = ctx->button_raw_state;
        ctx->button_last_change_ms = now_ms;
        if (ctx->button_stable_state){
            ctx->click_pending = true;
            ctx->last_click_ms = now_ms;
        }
    }
    return CEEPEW_OK;
}

CeePewErr_t input_get_char_index(const InputCtx_t *ctx, uint8_t total_items, uint8_t *out_index){
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out_index != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(total_items > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(ctx->initialised, CEEPEW_ERR_BUSY);
    uint32_t zone = ((uint32_t)ctx->smoothed_adc * (uint32_t)total_items) / ((uint32_t)CEEPEW_ADC_MAX_RAW + 1U);
    if (zone >= total_items){
        zone = (uint32_t)(total_items - 1U);
    }
    *out_index = (uint8_t)zone;
    return CEEPEW_OK;
}

CeePewErr_t input_consume_click(InputCtx_t *ctx, bool *was_clicked){
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(was_clicked != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(ctx->initialised, CEEPEW_ERR_BUSY);

    if (ctx->click_pending){
        ctx->click_pending = false;
        *was_clicked = true;
        return CEEPEW_OK;
    }
    *was_clicked = false;
    return CEEPEW_OK;
}

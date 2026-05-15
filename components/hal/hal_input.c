/* components/hal/hal_input.c */
#include "hal_input.h"
#include "hal_adc.h"
#include "../../main/ceepew_config.h"
#include "../../components/hal/hal_pins.h"
#include "driver/gpio.h"

/* Defaults for EMA if not defined elsewhere */
#ifndef CEEPEW_POT_EMA_ALPHA_NUM
#define CEEPEW_POT_EMA_ALPHA_NUM 1U
#endif
#ifndef CEEPEW_POT_EMA_ALPHA_DEN
#define CEEPEW_POT_EMA_ALPHA_DEN 4U
#endif

CeePewErr_t input_init(InputCtx_t *ctx)
{
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(!ctx->initialised, CEEPEW_ERR_BUSY);

    /* initialise hardware-readable fields */
    ctx->raw_adc = 0U;
    ctx->smoothed_adc = 0U;
    ctx->cursor_pos = 0U;
    ctx->at_left_edge = true;
    ctx->at_right_edge = false;
    ctx->edge_stable_since = 0U;
    ctx->last_click_ms = 0U;
    ctx->click_pending = false;
    ctx->diag_switch_active = false;
    ctx->button_stable_state = true; /* assume released (HIGH) */
    ctx->button_last_change_ms = 0U;

    /* seed smoothed_adc with a first ADC read */
    uint16_t raw = 0U;
    CeePewErr_t err = hal_adc_read_raw(&raw);
    if (err != CEEPEW_OK)
    {
        return err;
    }
    ctx->raw_adc = raw;
    ctx->smoothed_adc = raw;

    ctx->initialised = true;
    return CEEPEW_OK;
}

CeePewErr_t input_update(InputCtx_t *ctx, uint32_t now_ms)
{
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(ctx->initialised, CEEPEW_ERR_BUSY);

    uint16_t raw = 0U;
    CeePewErr_t err = hal_adc_read_raw(&raw);
    if (err != CEEPEW_OK)
    {
        return err;
    }
    ctx->raw_adc = raw;

    /* EMA smoothing: smoothed = (a*raw + (d-a)*prev) / d */
    uint32_t prev = (uint32_t)ctx->smoothed_adc;
    uint32_t sm = (uint32_t)CEEPEW_POT_EMA_ALPHA_NUM * (uint32_t)raw + (uint32_t)(CEEPEW_POT_EMA_ALPHA_DEN - CEEPEW_POT_EMA_ALPHA_NUM) * prev;
    sm /= (uint32_t)CEEPEW_POT_EMA_ALPHA_DEN;
    if (sm > CEEPEW_ADC_MAX_RAW)
    {
        sm = CEEPEW_ADC_MAX_RAW;
    }
    ctx->smoothed_adc = (uint16_t)sm;

    /* cursor position derived from ADC: 0..65535 mapped from 0..ADC_MAX */
    ctx->cursor_pos = (uint16_t)(((uint32_t)ctx->smoothed_adc * 65535U) / (uint32_t)CEEPEW_ADC_MAX_RAW);

    /* detect edges: left / right extremes (5% thresholds) */
    uint32_t left_thr = (uint32_t)CEEPEW_ADC_MAX_RAW / 20U; /* 5% */
    uint32_t right_thr = (uint32_t)CEEPEW_ADC_MAX_RAW - left_thr;
    bool left_now = (ctx->smoothed_adc <= left_thr);
    bool right_now = (ctx->smoothed_adc >= right_thr);
    if (left_now != ctx->at_left_edge || right_now != ctx->at_right_edge)
    {
        ctx->edge_stable_since = now_ms;
    }
    ctx->at_left_edge = left_now;
    ctx->at_right_edge = right_now;

    /* Read DIAG switch (polled, active LOW) */
    int diag_level = gpio_get_level(CEEPEW_PIN_DIAG_SWITCH);
    ctx->diag_switch_active = (diag_level == CEEPEW_DIAG_SWITCH_ACTIVE);

    /* Button debounce handling (active LOW) */
    int raw_button = gpio_get_level(CEEPEW_PIN_BUTTON);
    bool pressed = (raw_button == CEEPEW_BUTTON_ACTIVE_LEVEL);
    if (pressed != ctx->button_stable_state)
    {
        /* changed — check debounce window */
        if (now_ms - ctx->button_last_change_ms >= CEEPEW_BUTTON_DEBOUNCE_MS)
        {
            /* accept change */
            ctx->button_stable_state = pressed;
            ctx->button_last_change_ms = now_ms;
            if (pressed)
            {
                ctx->click_pending = true;
                ctx->last_click_ms = now_ms;
            }
        }
        else
        {
            /* not yet stable; update change timestamp */
            ctx->button_last_change_ms = now_ms;
        }
    }

    return CEEPEW_OK;
}

CeePewErr_t input_get_char_index(const InputCtx_t *ctx, uint8_t total_items, uint8_t *out_index)
{
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out_index != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(total_items > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(ctx->initialised, CEEPEW_ERR_BUSY);

    uint32_t zone = ((uint32_t)ctx->smoothed_adc * (uint32_t)total_items) / ((uint32_t)CEEPEW_ADC_MAX_RAW + 1U);
    if (zone >= total_items)
    {
        zone = (uint32_t)(total_items - 1U);
    }
    *out_index = (uint8_t)zone;
    return CEEPEW_OK;
}

CeePewErr_t input_consume_click(InputCtx_t *ctx, bool *was_clicked)
{
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(was_clicked != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(ctx->initialised, CEEPEW_ERR_BUSY);

    if (ctx->click_pending)
    {
        ctx->click_pending = false;
        *was_clicked = true;
        return CEEPEW_OK;
    }
    *was_clicked = false;
    return CEEPEW_OK;
}

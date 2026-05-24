/* components/hal/hal_input.h */
#ifndef HAL_INPUT_H
#define HAL_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "ceepew_assert.h"

typedef struct{
    uint16_t raw_adc;
    uint16_t smoothed_adc;
    uint16_t cursor_pos;
    bool at_left_edge;
    bool at_right_edge;
    uint32_t edge_stable_since;
    uint32_t last_click_ms;
    bool click_pending;
    bool diag_switch_active;
    /* internal state */
    bool initialised;
    bool button_raw_state;
    bool button_stable_state;
    uint32_t button_raw_change_ms;
    uint32_t button_last_change_ms;
} InputCtx_t;

CeePewErr_t input_init(InputCtx_t *ctx);
CeePewErr_t input_update(InputCtx_t *ctx, uint32_t now_ms);
CeePewErr_t input_get_char_index(const InputCtx_t *ctx, uint8_t total_items, uint8_t *out_index);
CeePewErr_t input_consume_click(InputCtx_t *ctx, bool *was_clicked);

/* Get normalized pot value (0-255), button stable state, and diag switch state. */
CeePewErr_t input_get_normalized(const InputCtx_t *ctx, uint8_t *out_pot, bool *out_button, bool *out_diag);

#endif /* HAL_INPUT_H */

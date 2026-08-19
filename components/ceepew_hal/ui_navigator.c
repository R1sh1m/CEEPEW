/* components/ceepew_hal/ui_navigator.c
 *
 * Pure-C relative navigation with hysteresis, velocity ramp, and edge dwell.
 * Zero heap, no ESP-IDF / FreeRTOS dependencies — host-compilable for unit tests.
 */

#include "ui_navigator.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <stddef.h>

/* Step threshold (pot units) — per CEEPEW_NAV_STEP_THRESH (default 4). */
#define NAV_STEP_THRESH          CEEPEW_NAV_STEP_THRESH

/* Velocity divisor: step = clamp(|dv| / VEL_DIV, 1, MAX_STEP).
 * Per CEEPEW_NAV_VEL_DIV (default 6), CEEPEW_NAV_MAX_STEP (default 8). */
#define NAV_VEL_DIV              CEEPEW_NAV_VEL_DIV
#define NAV_MAX_STEP             CEEPEW_NAV_MAX_STEP

/* Edge zone (pot 0..4 and 251..255) — per CEEPEW_NAV_EDGE_ZONE (default 4). */
#define NAV_EDGE_ZONE            CEEPEW_NAV_EDGE_ZONE

/* Edge dwell delay before auto-repeat starts (ms) — CEEPEW_NAV_EDGE_REPEAT_DELAY_MS (400). */
#define NAV_EDGE_REPEAT_DELAY_MS CEEPEW_NAV_EDGE_REPEAT_DELAY_MS

/* Auto-repeat tick interval once dwell is active (ms) — CEEPEW_NAV_EDGE_REPEAT_TICK_MS (80). */
#define NAV_EDGE_REPEAT_TICK_MS  CEEPEW_NAV_EDGE_REPEAT_TICK_MS

/* Max auto-repeat step multiplier during edge dwell — CEEPEW_NAV_MAX_STEP (8). */
#define NAV_EDGE_MAX_STEP        CEEPEW_NAV_MAX_STEP

/* Internal: clamp helper */
static inline int8_t nav_clamp8(int16_t v, int8_t lo, int8_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (int8_t)v;
}

static inline uint8_t nav_clamp_u8(int16_t v, uint8_t lo, uint8_t hi)
{
    if ((int16_t)v < (int16_t)lo) return lo;
    if ((int16_t)v > (int16_t)hi) return hi;
    return (uint8_t)v;
}

CeePewErr_t ui_nav_init(NavCtx_t *nav, uint8_t count, uint8_t initial)
{
    CEEPEW_ASSERT(nav != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(count > 0U, CEEPEW_ERR_PARAM);

    nav->count = count;
    nav->cursor = (initial < count) ? initial : (count - 1U);
    nav->anchor = 0U;
    nav->travel_accum = 0;
    nav->fast_accum = 0;
    nav->edge_repeat_start_ms = 0U;
    nav->last_update_ms = 0U;
    nav->hold_until_ms = 0U;

    return CEEPEW_OK;
}

CeePewErr_t ui_nav_reset(NavCtx_t *nav, uint8_t pot)
{
    CEEPEW_ASSERT(nav != NULL, CEEPEW_ERR_NULL_PTR);

    nav->anchor = pot;
    nav->travel_accum = 0;
    nav->fast_accum = 0;
    nav->edge_repeat_start_ms = 0U;
    nav->hold_until_ms = 0U;

    return CEEPEW_OK;
}

CeePewErr_t ui_nav_hold(NavCtx_t *nav, uint8_t pot, uint32_t hold_ms, uint32_t now_ms)
{
    CEEPEW_ASSERT(nav != NULL, CEEPEW_ERR_NULL_PTR);

    nav->anchor = pot;
    nav->travel_accum = 0;
    nav->fast_accum = 0;
    nav->edge_repeat_start_ms = 0U;
    nav->last_update_ms = now_ms;
    nav->hold_until_ms = (hold_ms > 0U) ? (now_ms + hold_ms) : 0U;

    return CEEPEW_OK;
}

CeePewErr_t ui_nav_set_count(NavCtx_t *nav, uint8_t count)
{
    CEEPEW_ASSERT(nav != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(count > 0U, CEEPEW_ERR_PARAM);

    nav->count = count;
    if (nav->cursor >= count) {
        nav->cursor = (count > 0U) ? (count - 1U) : 0U;
    }
    return CEEPEW_OK;
}

CeePewErr_t ui_nav_update(NavCtx_t *nav, uint8_t pot, uint32_t now_ms)
{
    CEEPEW_ASSERT(nav != NULL, CEEPEW_ERR_NULL_PTR);

    if (nav->count == 0U) {
        return CEEPEW_OK;
    }

    /* Post-selection hold: absorb button release torque / mechanical recoil */
    if (nav->hold_until_ms != 0U) {
        if (now_ms < nav->hold_until_ms) {
            nav->anchor = pot;
            nav->travel_accum = 0;
            nav->fast_accum = 0;
            nav->edge_repeat_start_ms = 0U;
            nav->last_update_ms = now_ms;
            return CEEPEW_OK;
        }
        /* Hold just expired: seed anchor to current sample so delta starts from 0 */
        nav->hold_until_ms = 0U;
        nav->anchor = pot;
        nav->travel_accum = 0;
        nav->fast_accum = 0;
        nav->edge_repeat_start_ms = 0U;
        nav->last_update_ms = now_ms;
        return CEEPEW_OK;
    }

    /* First call or timestamp wrap: seed anchor and timestamp, no stepping. */
    if (nav->last_update_ms == 0U) {
        nav->anchor = pot;
        nav->last_update_ms = now_ms;
        nav->edge_repeat_start_ms = 0U;
        return CEEPEW_OK;
    }

    /* Per-frame delta: compute before updating anchor so it represents
     * movement since the last sample. Reject full-rotation wraps. */
    int16_t diff = (int16_t)pot - (int16_t)nav->anchor;
    nav->anchor = pot;  /* store last sample for next frame */
    if (diff >= 128 || diff <= -128) {
        diff = 0;  /* spurious jump — ignore */
    }
    int8_t dv = (int8_t)diff;

    /* ---- Edge dwell detection ---- */
    bool at_left_edge  = (pot <= NAV_EDGE_ZONE);
    bool at_right_edge = (pot >= (255U - NAV_EDGE_ZONE));

    if (at_left_edge || at_right_edge) {
        if (nav->edge_repeat_start_ms == 0U) {
            nav->edge_repeat_start_ms = now_ms;
        } else {
            uint32_t dwell_ms = (now_ms >= nav->edge_repeat_start_ms)
                                ? (now_ms - nav->edge_repeat_start_ms)
                                : 0U;
            if (dwell_ms >= NAV_EDGE_REPEAT_DELAY_MS) {
                /* Check if it's time for the next auto-repeat tick. */
                uint32_t ticks = dwell_ms / NAV_EDGE_REPEAT_TICK_MS;
                if (ticks > 0U) {
                    /* Ramp step size: 1 -> 8 over successive ticks. */
                    uint8_t step = 1U + nav_clamp_u8((int16_t)ticks, 0U, NAV_EDGE_MAX_STEP - 1U);
                    if (step > NAV_EDGE_MAX_STEP) {
                        step = NAV_EDGE_MAX_STEP;
                    }
                    int8_t dir = at_left_edge ? -1 : +1;
                    int16_t new_cursor = (int16_t)nav->cursor + (int16_t)dir * (int16_t)step;
                    nav->cursor = nav_clamp_u8(new_cursor, 0U, nav->count - 1U);
                    /* Do NOT advance anchor — it already tracks the last sample.
                     * Reset sub-threshold accumulators. */
                    nav->travel_accum = 0;
                    nav->fast_accum = 0;
                }
            }
        }
    } else {
        nav->edge_repeat_start_ms = 0U;
    }

    /* ---- Velocity-based stepping (only when not in edge dwell) ---- */
    if (nav->edge_repeat_start_ms == 0U) {
        /* Accumulate sub-threshold travel using per-frame delta. */
        nav->travel_accum = nav_clamp8((int16_t)nav->travel_accum + dv, -127, 127);

        /* Velocity ramp uses the same per-frame |dv|. fast_accum is vestigial;
         * keep it zeroed for struct compatibility. */
        nav->fast_accum = 0;

        /* Check if accumulated travel crosses the threshold. */
        int8_t abs_travel = nav->travel_accum >= 0 ? nav->travel_accum : -nav->travel_accum;

        /* Velocity ramp: step multiplier = clamp(|dv| / VEL_DIV, 1, MAX_STEP). */
        int8_t abs_dv = dv >= 0 ? dv : -dv;
        uint8_t vel_mult = 1U + (uint8_t)(abs_dv / NAV_VEL_DIV);
        if (vel_mult > NAV_MAX_STEP) {
            vel_mult = NAV_MAX_STEP;
        }

        if (abs_travel >= NAV_STEP_THRESH) {
            /* Number of whole thresholds crossed. */
            uint8_t threshold_crossings = (uint8_t)(abs_travel / NAV_STEP_THRESH);

            /* Total steps = crossings * velocity multiplier. */
            uint8_t total_steps = threshold_crossings * vel_mult;

            int8_t dir = nav->travel_accum >= 0 ? +1 : -1;
            int16_t new_cursor = (int16_t)nav->cursor + (int16_t)dir * (int16_t)total_steps;
            nav->cursor = nav_clamp_u8(new_cursor, 0U, nav->count - 1U);

            /* Do NOT advance anchor — it tracks the latest sample.
             * Keep the remainder in travel_accum for sub-threshold precision. */
            nav->travel_accum = (int8_t)(abs_travel % NAV_STEP_THRESH);
            if (dir < 0) {
                nav->travel_accum = -nav->travel_accum;
            }
        }
    }

    nav->last_update_ms = now_ms;
    return CEEPEW_OK;
}

uint8_t ui_nav_get_index(const NavCtx_t *nav)
{
    CEEPEW_ASSERT(nav != NULL, CEEPEW_ERR_NULL_PTR);
    return nav->cursor;
}
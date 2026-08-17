/* components/ceepew_hal/ui_navigator.h
 *
 * Pure-C relative navigation with hysteresis, velocity ramp, and edge dwell.
 * Zero heap, no ESP-IDF / FreeRTOS dependencies — host-compilable for unit tests.
 */

#ifndef UI_NAVIGATOR_H
#define UI_NAVIGATOR_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Navigator context — fits in UIContext_t, no dynamic allocation. */
typedef struct {
    uint8_t  count;                   /* number of selectable items (0 = disabled) */
    uint8_t  cursor;                  /* current index 0 .. count-1 */
    uint8_t  anchor;                  /* pot value (0-255) at last committed step */
    int8_t   travel_accum;            /* accumulated sub-threshold travel (-128..127) */
    int8_t   fast_accum;              /* per-frame delta window for velocity ramp */
    uint32_t edge_repeat_start_ms;    /* ms when pot entered edge zone; 0 = not in edge */
    uint32_t last_update_ms;          /* timestamp of last update for delta time */
    uint32_t hold_until_ms;           /* timestamp (ms) until which position is held post-selection */
} NavCtx_t;

/* Initialize navigator for a given item count.
 * Sets cursor = initial (clamped), anchor = 0 (will be seeded on first update).
 * Returns CEEPEW_OK or CEEPEW_ERR_PARAM if count == 0. */
CeePewErr_t ui_nav_init(NavCtx_t *nav, uint8_t count, uint8_t initial);

/* Reset anchor to current pot without moving cursor.
 * Call on state entry or when advancing code digits. */
CeePewErr_t ui_nav_reset(NavCtx_t *nav, uint8_t pot);

/* Hold cursor at current position for hold_ms milliseconds.
 * Re-anchors to current pot and clears accumulators to eliminate post-click jitter. */
CeePewErr_t ui_nav_hold(NavCtx_t *nav, uint8_t pot, uint32_t hold_ms, uint32_t now_ms);

/* Update navigator with current pot value (0-255) and timestamp (ms).
 * Applies hysteresis threshold, velocity ramp, and edge-dwell auto-repeat.
 * Must be called once per UI frame (~60 Hz). */
CeePewErr_t ui_nav_update(NavCtx_t *nav, uint8_t pot, uint32_t now_ms);

/* Get current cursor index (0 .. count-1). Returns 0 if count == 0. */
uint8_t ui_nav_get_index(const NavCtx_t *nav);

/* Set item count dynamically (e.g., message list shrinks on TTL expiry).
 * Clamps cursor to new range. */
CeePewErr_t ui_nav_set_count(NavCtx_t *nav, uint8_t count);

#ifdef __cplusplus
}
#endif

#endif /* UI_NAVIGATOR_H */
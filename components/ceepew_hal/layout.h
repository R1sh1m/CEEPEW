/* components/ceepew_hal/layout.h */

#ifndef CEEPEW_LAYOUT_H
#define CEEPEW_LAYOUT_H

#include "ceepew_assert.h"
#include "hal_ui.h"
#include "ui_manager.h"

/* Validate compile-time layout tables for every UI state.
 * This ensures no overlap with the reserved status/navigation bars and that
 * all per-state regions fit inside the 128×64 display. Called at boot.
 */
CeePewErr_t layout_validate_state_regions(void);
CeePewErr_t layout_validate_state_entry(UIState_t state);
const char *layout_state_name(UIState_t state);

#endif /* CEEPEW_LAYOUT_H */

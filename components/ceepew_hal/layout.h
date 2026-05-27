/* components/ceepew_hal/layout.h */

#ifndef CEEPEW_LAYOUT_H
#define CEEPEW_LAYOUT_H

#include "ceepew_assert.h"

/* Validate compile-time layout tables for every UI state.
 * This ensures no overlap with the reserved status/navigation bars and that
 * all per-state regions fit inside the 128×64 display. Called at boot.
 */
CeePewErr_t layout_validate_state_regions(void);

#endif /* CEEPEW_LAYOUT_H */

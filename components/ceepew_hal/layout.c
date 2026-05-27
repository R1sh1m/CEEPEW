/* components/ceepew_hal/layout.c */

#include "layout.h"
#include "hal_ui.h"
#include "ui_manager.h"
#include "ceepew_assert.h"
#include <stdbool.h>

/* Design note: For a small fixed display, prefer static per-state region
   tables that are validated once at boot. This avoids a runtime constraint
   solver, keeps memory bounded, and makes overlap violations detectable at
   initialization rather than during user interactions. */

static bool rects_overlap(const HalUIRect_t *a, const HalUIRect_t *b)
{
    if (a->x >= (uint16_t)(b->x + b->w)) { return false; }
    if (b->x >= (uint16_t)(a->x + a->w)) { return false; }
    if (a->y >= (uint16_t)(b->y + b->h)) { return false; }
    if (b->y >= (uint16_t)(a->y + a->h)) { return false; }
    return true;
}

/* Global reserved regions (invariant) */
static const HalUIRect_t s_global_regions[] = {
    { .x = 0U,  .y = 0U,  .w = HAL_UI_WIDTH_PX,  .h = 8U },   /* status bar */
    { .x = 0U,  .y = 56U, .w = HAL_UI_WIDTH_PX,  .h = 8U }    /* nav bar */
};

/* Per-state static region tables (only content-area regions, y anchored at 8px) */
static const HalUIRect_t s_state_discovery[] = {
    { .x = 4U,  .y = 8U,  .w = 56U,  .h = 48U }, /* radar */
    { .x = 64U, .y = 8U,  .w = 60U,  .h = 48U }  /* right panel */
};

static const HalUIRect_t s_state_code_entry[] = {
    { .x = 6U,  .y = 16U, .w = 116U, .h = 36U }  /* code entry box */
};

static const HalUIRect_t s_state_countdown[] = {
    { .x = 6U,  .y = 12U, .w = 116U, .h = 32U }, /* big countdown area */
    { .x = 6U,  .y = 44U, .w = 116U, .h = 8U }   /* progress / hint row */
};

static const HalUIRect_t s_state_fingerprint[] = {
    { .x = 8U,  .y = 12U, .w = 112U, .h = 32U }, /* fingerprint hex / icon */
    { .x = 8U,  .y = 48U, .w = 112U, .h = 8U }   /* action hint row */
};

static const HalUIRect_t s_state_cryptogram[] = {
    { .x = 10U, .y = 12U, .w = 108U, .h = 36U }  /* hex grid / cryptogram */
};

static const HalUIRect_t s_state_chat_menu[] = {
    { .x = 8U,  .y = 20U, .w = 112U, .h = 36U }  /* two selectable rows */
};

static const HalUIRect_t s_state_chat_compose[] = {
    { .x = 0U,  .y = 8U,  .w = 128U, .h = 16U }, /* header */
    { .x = 8U,  .y = 24U, .w = 112U, .h = 24U }  /* compose area */
};

static const HalUIRect_t s_state_error[] = {
    { .x = 8U,  .y = 12U, .w = 112U, .h = 36U }  /* error message box */
};

/* Registry of per-state tables to validate. If a state is omitted here it
   is assumed to have no content-area regions beyond status/nav bars. */
typedef struct {
    const char *name;
    const HalUIRect_t *rects;
    uint8_t count;
} StateLayoutDef_t;

static const StateLayoutDef_t s_layouts[] = {
    { "DISCOVERY",  s_state_discovery,  (uint8_t)(sizeof(s_state_discovery) / sizeof(HalUIRect_t)) },
    { "CODE_ENTRY",  s_state_code_entry,  (uint8_t)(sizeof(s_state_code_entry) / sizeof(HalUIRect_t)) },
    { "COUNTDOWN",   s_state_countdown,   (uint8_t)(sizeof(s_state_countdown) / sizeof(HalUIRect_t)) },
    { "FINGERPRINT", s_state_fingerprint, (uint8_t)(sizeof(s_state_fingerprint) / sizeof(HalUIRect_t)) },
    { "CRYPTOGRAM",  s_state_cryptogram,  (uint8_t)(sizeof(s_state_cryptogram) / sizeof(HalUIRect_t)) },
    { "CHAT_MENU",   s_state_chat_menu,   (uint8_t)(sizeof(s_state_chat_menu) / sizeof(HalUIRect_t)) },
    { "CHAT_COMPOSE",s_state_chat_compose,(uint8_t)(sizeof(s_state_chat_compose) / sizeof(HalUIRect_t)) },
    { "ERROR",       s_state_error,       (uint8_t)(sizeof(s_state_error) / sizeof(HalUIRect_t)) }
};

CeePewErr_t layout_validate_state_regions(void)
{
    /* Validate global regions fit on-screen */
    for (uint8_t g = 0U; g < (uint8_t)(sizeof(s_global_regions) / sizeof(HalUIRect_t)); g++) {
        const HalUIRect_t *gr = &s_global_regions[g];
        CEEPEW_ASSERT((uint16_t)gr->x + gr->w <= HAL_UI_WIDTH_PX, CEEPEW_ERR_BOUNDS);
        CEEPEW_ASSERT((uint16_t)gr->y + gr->h <= HAL_UI_HEIGHT_PX, CEEPEW_ERR_BOUNDS);
    }

    /* Validate each state's regions: inside display, no overlap with globals,
       and no intra-state overlaps. */
    for (uint8_t s = 0U; s < (uint8_t)(sizeof(s_layouts) / sizeof(StateLayoutDef_t)); s++) {
        const StateLayoutDef_t *sd = &s_layouts[s];
        for (uint8_t i = 0U; i < sd->count; i++) {
            const HalUIRect_t *r = &sd->rects[i];
            CEEPEW_ASSERT(r->w > 0U && r->h > 0U, CEEPEW_ERR_BOUNDS);
            CEEPEW_ASSERT((uint16_t)r->x + r->w <= HAL_UI_WIDTH_PX, CEEPEW_ERR_BOUNDS);
            CEEPEW_ASSERT((uint16_t)r->y + r->h <= HAL_UI_HEIGHT_PX, CEEPEW_ERR_BOUNDS);

            /* Check against globals */
            for (uint8_t g = 0U; g < (uint8_t)(sizeof(s_global_regions) / sizeof(HalUIRect_t)); g++) {
                if (rects_overlap(r, &s_global_regions[g])) {
                    return CEEPEW_ERR_BOUNDS;
                }
            }

            /* Intra-state overlaps */
            for (uint8_t j = (uint8_t)(i + 1U); j < sd->count; j++) {
                if (rects_overlap(r, &sd->rects[j])) {
                    return CEEPEW_ERR_BOUNDS;
                }
            }
        }
    }

    return CEEPEW_OK;
}

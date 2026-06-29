/* components/ceepew_hal/layout.c */

#include "layout.h"
#include "ceepew_assert.h"
#include "esp_log.h"
#include <stdbool.h>

/* Design note: The guard now validates state layouts once on entry instead of
   on every draw call. That keeps the render loop cheap while still catching
   real layout regressions with a state name and the exact zone pair involved. */

static const char *TAG = "layout";

typedef struct {
    const char *name;
    HalUIRect_t rect;
} LayoutZone_t;

typedef struct {
    UIState_t state;
    const char *name;
    const LayoutZone_t *zones;
    uint8_t zone_count;
} LayoutState_t;

static bool rects_overlap(const HalUIRect_t *a, const HalUIRect_t *b)
{
    uint16_t a_x2 = (uint16_t)a->x + (uint16_t)a->w;
    uint16_t b_x2 = (uint16_t)b->x + (uint16_t)b->w;
    uint16_t a_y2 = (uint16_t)a->y + (uint16_t)a->h;
    uint16_t b_y2 = (uint16_t)b->y + (uint16_t)b->h;

    if ((uint16_t)a->x >= b_x2) { return false; }
    if ((uint16_t)b->x >= a_x2) { return false; }
    if ((uint16_t)a->y >= b_y2) { return false; }
    if ((uint16_t)b->y >= a_y2) { return false; }
    return true;
}

static bool rect_within_screen(const HalUIRect_t *r)
{
    return ((uint16_t)r->x + (uint16_t)r->w <= HAL_UI_WIDTH_PX) &&
           ((uint16_t)r->y + (uint16_t)r->h <= HAL_UI_HEIGHT_PX) &&
           (r->w > 0U) && (r->h > 0U);
}

static const LayoutZone_t s_boot_zones[] = {
    { "full", { .x = 0U, .y = 0U, .w = 128U, .h = 64U } },
};

static const LayoutZone_t s_discovery_zones[] = {
    { "radar-panel",   { .x = 0U,  .y = 0U, .w = 50U, .h = 64U } },
    { "info-panel",    { .x = 53U, .y = 0U, .w = 75U, .h = 64U } },
};

static const LayoutZone_t s_code_entry_zones[] = {
    { "title",         { .x = 0U,  .y = 0U,  .w = 128U, .h = 12U } },
    { "code-grid",     { .x = 0U,  .y = 12U, .w = 128U, .h = 26U } },
    { "selector",      { .x = 0U,  .y = 38U, .w = 128U, .h = 10U } },
    { "footer",        { .x = 0U,  .y = 48U, .w = 128U, .h = 16U } },
};

static const LayoutZone_t s_countdown_zones[] = {
    { "title",         { .x = 0U,  .y = 0U,  .w = 128U, .h = 12U } },
    { "code",          { .x = 0U,  .y = 12U, .w = 128U, .h = 25U } },
    { "progress",      { .x = 0U,  .y = 37U, .w = 128U, .h = 8U } },
    { "hint",          { .x = 0U,  .y = 45U, .w = 128U, .h = 19U } },
};

static const LayoutZone_t s_pairing_result_zones[] = {
    { "title",         { .x = 0U,  .y = 0U,  .w = 128U, .h = 12U } },
    { "message",       { .x = 0U,  .y = 12U, .w = 128U, .h = 28U } },
    { "footer",        { .x = 0U,  .y = 40U, .w = 128U, .h = 24U } },
};

static const LayoutZone_t s_confirm_zones[] = {
    { "title",         { .x = 0U,  .y = 0U,  .w = 128U, .h = 12U } },
    { "code_display",  { .x = 0U,  .y = 12U, .w = 128U, .h = 20U } },
    { "status",        { .x = 0U,  .y = 32U, .w = 128U, .h = 16U } },
    { "prompt",        { .x = 0U,  .y = 48U, .w = 128U, .h = 16U } },
};

static const LayoutZone_t s_keyder_zones[] = {
    { "matrix",        { .x = 0U,  .y = 0U,  .w = 128U, .h = 50U } },
    { "progress",      { .x = 0U,  .y = 50U, .w = 128U, .h = 12U } },
};

static const LayoutZone_t s_chat_zones[] = {
    { "title",         { .x = 0U,  .y = 0U,  .w = 128U, .h = 12U } },
    { "thread",        { .x = 0U,  .y = 12U, .w = 128U, .h = 42U } },
    { "footer",        { .x = 0U,  .y = 54U, .w = 128U, .h = 10U } },
};

static const LayoutZone_t s_chat_compose_zones[] = {
    { "status",        { .x = 0U,  .y = 0U,  .w = 128U, .h = 10U } },
    { "selector",      { .x = 0U,  .y = 10U, .w = 128U, .h = 24U } },
    { "preview",       { .x = 0U,  .y = 34U, .w = 128U, .h = 13U } },
    { "legend",        { .x = 0U,  .y = 47U, .w = 128U, .h = 17U } },
};

static const LayoutZone_t s_chat_send_confirm_zones[] = {
    { "title",         { .x = 0U,  .y = 0U,  .w = 128U, .h = 12U } },
    { "message",       { .x = 0U,  .y = 12U, .w = 128U, .h = 26U } },
    { "choices",       { .x = 0U,  .y = 38U, .w = 128U, .h = 18U } },
    { "footer",        { .x = 0U,  .y = 56U, .w = 128U, .h = 8U } },
};

static const LayoutZone_t s_cryptogram_zones[] = {
    { "title",         { .x = 0U,  .y = 0U,  .w = 128U, .h = 12U } },
    { "code",          { .x = 0U,  .y = 12U, .w = 128U, .h = 38U } },
    { "status",        { .x = 0U,  .y = 50U, .w = 128U, .h = 14U } },
};

static const LayoutZone_t s_nonce_zones[] = {
    { "title",         { .x = 0U,  .y = 0U,  .w = 128U, .h = 12U } },
    { "body",          { .x = 0U,  .y = 12U, .w = 128U, .h = 52U } },
};

static const LayoutZone_t s_info_zones[] = {
    { "title",         { .x = 0U,  .y = 0U,  .w = 128U, .h = 12U } },
    { "body",          { .x = 0U,  .y = 12U, .w = 128U, .h = 44U } },
    { "footer",        { .x = 0U,  .y = 56U, .w = 128U, .h = 8U } },
};

static const LayoutZone_t s_error_zones[] = {
    { "full",          { .x = 0U,  .y = 0U,  .w = 128U, .h = 64U } },
};

static const LayoutState_t s_states[] = {
    { UI_STATE_BOOT,               "BOOT",               s_boot_zones,           (uint8_t)(sizeof(s_boot_zones) / sizeof(s_boot_zones[0])) },
    { UI_STATE_DISCOVERY,          "DISCOVERY",          s_discovery_zones,      (uint8_t)(sizeof(s_discovery_zones) / sizeof(s_discovery_zones[0])) },
    { UI_STATE_CODE_ENTRY,         "CODE_ENTRY",         s_code_entry_zones,     (uint8_t)(sizeof(s_code_entry_zones) / sizeof(s_code_entry_zones[0])) },
    { UI_STATE_COUNTDOWN,          "COUNTDOWN",          s_countdown_zones,      (uint8_t)(sizeof(s_countdown_zones) / sizeof(s_countdown_zones[0])) },
    /* 4U, 5U reserved */
    { UI_STATE_CONFIRM,            "CONFIRM",            s_confirm_zones,        (uint8_t)(sizeof(s_confirm_zones) / sizeof(s_confirm_zones[0])) },
    { UI_STATE_KEYDER,             "KEYDER",             s_keyder_zones,         (uint8_t)(sizeof(s_keyder_zones) / sizeof(s_keyder_zones[0])) },
    { UI_STATE_CHAT,               "CHAT",               s_chat_zones,           (uint8_t)(sizeof(s_chat_zones) / sizeof(s_chat_zones[0])) },
    { UI_STATE_CHAT_MENU,          "CHAT_MENU",          s_chat_zones,           (uint8_t)(sizeof(s_chat_zones) / sizeof(s_chat_zones[0])) },
    { UI_STATE_CHAT_COMPOSE,       "CHAT_COMPOSE",       s_chat_compose_zones,   (uint8_t)(sizeof(s_chat_compose_zones) / sizeof(s_chat_compose_zones[0])) },
    { UI_STATE_CHAT_SEND_CONFIRM,  "CHAT_SEND_CONFIRM",  s_chat_send_confirm_zones, (uint8_t)(sizeof(s_chat_send_confirm_zones) / sizeof(s_chat_send_confirm_zones[0])) },
    { UI_STATE_CHAT_DETAIL,        "CHAT_DETAIL",        s_chat_zones,           (uint8_t)(sizeof(s_chat_zones) / sizeof(s_chat_zones[0])) },
    { UI_STATE_CRYPTOGRAM,         "CRYPTOGRAM",         s_cryptogram_zones,     (uint8_t)(sizeof(s_cryptogram_zones) / sizeof(s_cryptogram_zones[0])) },
    { UI_STATE_NONCE_EXHAUSTED,    "NONCE_EXHAUSTED",    s_nonce_zones,          (uint8_t)(sizeof(s_nonce_zones) / sizeof(s_nonce_zones[0])) },
    { UI_STATE_INFO,               "INFO",               s_info_zones,           (uint8_t)(sizeof(s_info_zones) / sizeof(s_info_zones[0])) },
    { UI_STATE_ERROR,              "ERROR",              s_error_zones,          (uint8_t)(sizeof(s_error_zones) / sizeof(s_error_zones[0])) },
    { UI_STATE_PAIRING,            "PAIRING",            s_countdown_zones,      (uint8_t)(sizeof(s_countdown_zones) / sizeof(s_countdown_zones[0])) },
    { UI_STATE_PAIRING_FAILED,     "PAIRING_FAILED",     s_pairing_result_zones, (uint8_t)(sizeof(s_pairing_result_zones) / sizeof(s_pairing_result_zones[0])) },
};

static const LayoutState_t *layout_for_state(UIState_t state)
{
    for (uint8_t i = 0U; i < (uint8_t)(sizeof(s_states) / sizeof(s_states[0])); i++) {
        if (s_states[i].state == state) {
            return &s_states[i];
        }
    }
    return NULL;
}

const char *layout_state_name(UIState_t state)
{
    const LayoutState_t *ls = layout_for_state(state);
    return (ls != NULL) ? ls->name : "UNKNOWN";
}

CeePewErr_t layout_validate_state_entry(UIState_t state)
{
    const LayoutState_t *ls = layout_for_state(state);
    CEEPEW_ASSERT(ls != NULL, CEEPEW_ERR_PARAM);

    ESP_LOGI(TAG, "state-entry validate: %s zones=%u", ls->name, (unsigned)ls->zone_count);

    for (uint8_t i = 0U; i < ls->zone_count; i++) {
        const LayoutZone_t *zi = &ls->zones[i];
        CEEPEW_ASSERT(rect_within_screen(&zi->rect), CEEPEW_ERR_BOUNDS);
        for (uint8_t j = (uint8_t)(i + 1U); j < ls->zone_count; j++) {
            const LayoutZone_t *zj = &ls->zones[j];
            if (rects_overlap(&zi->rect, &zj->rect)) {
                ESP_LOGE(TAG,
                         "state=%s overlap: %s[%u,%u %u,%u] vs %s[%u,%u %u,%u]",
                         ls->name,
                         zi->name,
                         (unsigned)zi->rect.x, (unsigned)zi->rect.y,
                         (unsigned)zi->rect.w, (unsigned)zi->rect.h,
                         zj->name,
                         (unsigned)zj->rect.x, (unsigned)zj->rect.y,
                         (unsigned)zj->rect.w, (unsigned)zj->rect.h);
                return CEEPEW_ERR_BOUNDS;
            }
        }
    }

    return CEEPEW_OK;
}

CeePewErr_t layout_validate_state_regions(void)
{
    for (uint8_t i = 0U; i < (uint8_t)(sizeof(s_states) / sizeof(s_states[0])); i++) {
        CeePewErr_t err = layout_validate_state_entry(s_states[i].state);
        if (err != CEEPEW_OK) {
            return err;
        }
    }
    return CEEPEW_OK;
}

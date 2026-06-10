/* components/ceepew_hal/ui_manager.c
 * UI Manager Implementation — handles all remaining screen states and transitions.
 */

#include "ui_manager.h"
#include "hal_ui.h"
#include "hal_rgb.h"
#include "layout.h"
#include "../transport/transport_ble.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "ceepew_security_utils.h"
#include "../../main/session_fsm.h"
#include "../../main/session_msgstore.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_system.h"   /* esp_get_free_heap_size() */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

/* Design note: The UI manager is a simple state machine that tracks the
   current screen, animation state, and user input. Each screen state calls
   its corresponding render function. Transitions between screens are driven
   by completion flags and user input. This architecture allows all 9 screen
   types to coexist in one efficient state machine. */

/* Strong definition of g_ble_ctx lives in components/transport/transport_ble.c;
 * tests/component/ceepew_hal/ui_manager_test.c writes to it directly. Forward
 * declaration here — the weak fallback was a code smell (linked objects always
 * resolve to the strong symbol from transport_ble.c). */
extern BleContext_t g_ble_ctx;

/* Forward declarations of session FSM accessors for Sprint 10 integration.
 * Definitions in main/session_fsm.c; test injection via session_test_set_*()
 * setters in session_fsm.h. */
extern uint64_t session_get_id(void);
extern uint64_t session_get_nonce_counter(void);
extern CeePewErr_t session_get_commitment(uint8_t commitment[CEEPEW_COMMITMENT_BYTES]);
extern CeePewErr_t session_get_fingerprint(uint8_t fingerprint[16]);

UIContext_t g_ui_ctx = {0};
static bool s_ui_manager_initialised = false;
static uint32_t s_last_ui_wipe_ms = 0U;
static uint64_t s_diag_prev_draw_start_us = 0ULL;
static uint32_t s_diag_last_draw_cost_us = 0U;
static uint32_t s_diag_last_loop_rate_hz = 0U;
static const char *const s_diag_page_names[] = {
    "CPU&MEM",
    "TASKS",
    "STORAGE",
    "RUNTIME",
};
static const uint8_t s_diag_page_count =
    (uint8_t)(sizeof(s_diag_page_names) / sizeof(s_diag_page_names[0]));

static CeePewErr_t render_fingerprint_confirm(void);
static CeePewErr_t render_error(void);
static CeePewErr_t render_code_incorrect(void);
static CeePewErr_t render_info(void);
static CeePewErr_t render_code_different(void);
static CeePewErr_t render_pairing_success(void);
static CeePewErr_t render_pairing_failed(void);
static CeePewErr_t render_chat_send_confirm(void);
static CeePewErr_t ui_restart_discovery_from_pairing(void);

#define CEEPEW_PAIRING_SUCCESS_HOLD_MS  1200U
#define CEEPEW_PAIRING_FAILED_HOLD_MS   4000U    /* Bug 5 Fix: reduced from 15000 to allow reconnection */
#define CEEPEW_PAIRING_PEER_LOSS_MS     15000U
#define CEEPEW_SCAN_RETRY_DEBOUNCE_MS   2000U

CeePewErr_t ui_manager_init(void){
    CEEPEW_ASSERT(!s_ui_manager_initialised, CEEPEW_ERR_BUSY);
    memset(&g_ui_ctx, 0U, sizeof(UIContext_t));
    g_ui_ctx.current_state = UI_STATE_BOOT;
    g_ui_ctx.next_state = UI_STATE_BOOT;
    g_ui_ctx.anim.frame_count = 0U;
    g_ui_ctx.anim.frame_rate_ms = CEEPEW_UI_LOOP_DELAY_MS;  /* align with UI loop */
    g_ui_ctx.anim.active = true;
    g_ui_ctx.last_draw_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    g_ui_ctx.user_input = 0U;
    g_ui_ctx.button_pressed = false;
    g_ui_ctx.diag_mode = false;
    g_ui_ctx.transition_ready = false;
    g_ui_ctx.code_entry_start_ms = 0U;
    s_ui_manager_initialised = true;

    CeePewErr_t err = layout_validate_state_regions();
    CEEPEW_ASSERT(err == CEEPEW_OK, err);

    return CEEPEW_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1 — DRAW PRIMITIVES
 * All coordinate arguments are clamped; nothing writes outside 128×64.
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline void draw_pixel(uint8_t x, uint8_t y)
{
    if (x >= 128U || y >= 64U) { return; }
    HalUIRect_t r = { .x = x, .y = y, .w = 1U, .h = 1U };
    hal_ui_rect_fill(&r, HAL_UI_WHITE);
}

static void draw_hline(uint8_t x, uint8_t y, uint8_t w)
{
    if (x >= 128U || y >= 64U || w == 0U) { return; }
    if ((uint16_t)x + w > 128U) { w = (uint8_t)(128U - x); }
    HalUIRect_t r = { .x = x, .y = y, .w = w, .h = 1U };
    hal_ui_rect_fill(&r, HAL_UI_WHITE);
}

static void draw_vline(uint8_t x, uint8_t y, uint8_t h)
{
    if (x >= 128U || y >= 64U || h == 0U) { return; }
    if ((uint16_t)y + h > 64U) { h = (uint8_t)(64U - y); }
    HalUIRect_t r = { .x = x, .y = y, .w = 1U, .h = h };
    hal_ui_rect_fill(&r, HAL_UI_WHITE);
}

/* Bresenham line — safe, bounded, max 200 steps */
static void draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    int16_t dx  = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int16_t dy  = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int16_t sx  = (x0 < x1) ? 1 : -1;
    int16_t sy  = (y0 < y1) ? 1 : -1;
    int16_t err = dx - dy;

    for (uint8_t guard = 0U; guard < 200U; guard++) {
        if (x0 >= 0 && x0 < 128 && y0 >= 0 && y0 < 64) {
            draw_pixel((uint8_t)x0, (uint8_t)y0);
        }
        if (x0 == x1 && y0 == y1) { break; }
        int16_t e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Midpoint circle — outline only */
static void draw_circle(int16_t cx, int16_t cy, uint8_t r)
{
    CEEPEW_ASSERT_VOID(cx >= 0 && cy >= 0);
    CEEPEW_ASSERT_VOID((int32_t)cx - (int32_t)r >= 0);
    CEEPEW_ASSERT_VOID((int32_t)cy - (int32_t)r >= 0);
    CEEPEW_ASSERT_VOID((int32_t)cx + (int32_t)r < (int32_t)CEEPEW_OLED_WIDTH_PX);
    CEEPEW_ASSERT_VOID((int32_t)cy + (int32_t)r < (int32_t)CEEPEW_OLED_HEIGHT_PX);
    int16_t x = 0;
    int16_t y = (int16_t)r;
    int16_t d = 1 - (int16_t)r;

    while (x <= y) {
        draw_pixel((uint8_t)(cx + x), (uint8_t)(cy - y));
        draw_pixel((uint8_t)(cx - x), (uint8_t)(cy - y));
        draw_pixel((uint8_t)(cx + x), (uint8_t)(cy + y));
        draw_pixel((uint8_t)(cx - x), (uint8_t)(cy + y));
        draw_pixel((uint8_t)(cx + y), (uint8_t)(cy - x));
        draw_pixel((uint8_t)(cx - y), (uint8_t)(cy - x));
        draw_pixel((uint8_t)(cx + y), (uint8_t)(cy + x));
        draw_pixel((uint8_t)(cx - y), (uint8_t)(cy + x));
        if (d < 0) { d += 2 * x + 3; }
        else        { d += 2 * (x - y) + 5; y--; }
        x++;
    }
}

/* Partial sweep-sector fill: draws pixels between two radii at a single
 * angle defined by endpoint (ex, ey) from centre (cx, cy).
 * r_near..r_far: the radial band to fill (for trail gradient effect).
 * Implemented as a line segment from the near point to the far point. */
static void draw_radial_segment(int16_t cx, int16_t cy, int16_t ex, int16_t ey, uint8_t r_near, uint8_t r_far){
    /* Interpolate near point along the (cx,cy)→(ex,ey) vector */
    /* Full length is r_far (outer ring radius) */
    /* near point: r_near/r_far fraction along the arm */
    if (r_far == 0U) { return; }
    int16_t dx = ex - cx;
    int16_t dy = ey - cy;
    int16_t nx = cx + (int16_t)((int32_t)dx * r_near / r_far);
    int16_t ny = cy + (int16_t)((int32_t)dy * r_near / r_far);
    draw_line(nx, ny, ex, ey);
}

/* RSSI dBm → bar count 1–5 */
static uint8_t rssi_to_bars(int8_t rssi){
    if (rssi >= -50) { return 5U; }
    if (rssi >= -60) { return 4U; }
    if (rssi >= -70) { return 3U; }
    if (rssi >= -80) { return 2U; }
    return 1U;
}

typedef enum {
    COMPOSE_ACTION_CHAR = 0U,
    COMPOSE_ACTION_DEL = 1U,
    COMPOSE_ACTION_LEFT = 2U,
    COMPOSE_ACTION_RIGHT = 3U,
} ComposeAction_t;

#define COMPOSE_CHAR_COUNT        62U
#define COMPOSE_ACTION_DEL_IDX    62U
#define COMPOSE_ACTION_LEFT_IDX   63U
#define COMPOSE_ACTION_RIGHT_IDX  64U
#define COMPOSE_TOTAL_CHOICES     65U

static const char COMPOSE_CHARSET[COMPOSE_CHAR_COUNT + 1U] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    " .,!?;:'\"_-@#/()+=%&*<>";

static const char *const s_compose_special_labels[3U] = {
    "DEL",
    "<-",
    "->",
};

static const char *compose_choice_label(uint8_t choice_idx, char label_buf[4U])
{
    if (choice_idx < COMPOSE_CHAR_COUNT) {
        label_buf[0] = COMPOSE_CHARSET[choice_idx];
        label_buf[1] = '\0';
        return label_buf;
    }

    if (choice_idx == COMPOSE_ACTION_DEL_IDX) {
        return s_compose_special_labels[0];
    }
    if (choice_idx == COMPOSE_ACTION_LEFT_IDX) {
        return s_compose_special_labels[1];
    }
    if (choice_idx == COMPOSE_ACTION_RIGHT_IDX) {
        return s_compose_special_labels[2];
    }

    label_buf[0] = '?';
    label_buf[1] = '\0';
    return label_buf;
}

static void compose_terminate_buffer(void)
{
    uint16_t clamp_len = (uint16_t)g_ui_ctx.compose_length;
    g_ui_ctx.compose_buffer[clamp_len] = '\0';
}

static void compose_reset_cursor_if_needed(void)
{
    if (g_ui_ctx.compose_cursor > g_ui_ctx.compose_length) {
        g_ui_ctx.compose_cursor = g_ui_ctx.compose_length;
    }
}

static CeePewErr_t compose_insert_char(char ch)
{
    CEEPEW_ASSERT(g_ui_ctx.compose_length < 255U, CEEPEW_ERR_BOUNDS);

    if (g_ui_ctx.compose_length >= 255U) {
        return CEEPEW_ERR_BOUNDS;
    }

    compose_reset_cursor_if_needed();

    for (uint8_t i = g_ui_ctx.compose_length; i > g_ui_ctx.compose_cursor; i--) {
        g_ui_ctx.compose_buffer[i] = g_ui_ctx.compose_buffer[(uint8_t)(i - 1U)];
    }
    g_ui_ctx.compose_buffer[g_ui_ctx.compose_cursor] = ch;
    g_ui_ctx.compose_length++;
    g_ui_ctx.compose_cursor++;
    compose_terminate_buffer();
    return CEEPEW_OK;
}

static CeePewErr_t compose_delete_before_cursor(void)
{
    if (g_ui_ctx.compose_length == 0U || g_ui_ctx.compose_cursor == 0U) {
        return CEEPEW_OK;
    }

    compose_reset_cursor_if_needed();
    uint8_t delete_idx = (uint8_t)(g_ui_ctx.compose_cursor - 1U);
    for (uint8_t i = delete_idx; (uint8_t)(i + 1U) < g_ui_ctx.compose_length; i++) {
        g_ui_ctx.compose_buffer[i] = g_ui_ctx.compose_buffer[(uint8_t)(i + 1U)];
    }
    g_ui_ctx.compose_length--;
    g_ui_ctx.compose_cursor--;
    compose_terminate_buffer();
    return CEEPEW_OK;
}

static CeePewErr_t compose_move_cursor_left(void)
{
    if (g_ui_ctx.compose_cursor > 0U) {
        g_ui_ctx.compose_cursor--;
    }
    return CEEPEW_OK;
}

static CeePewErr_t compose_move_cursor_right(void)
{
    if (g_ui_ctx.compose_cursor < g_ui_ctx.compose_length) {
        g_ui_ctx.compose_cursor++;
    }
    return CEEPEW_OK;
}

static void draw_selected_option_row(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                                     const char *text, bool selected)
{
    HalUIRect_t box = { .x = x, .y = y, .w = w, .h = h };
    if (selected) {
        hal_ui_rect_fill(&box, HAL_UI_WHITE);
        hal_ui_rect(&box, HAL_UI_WHITE);
        hal_ui_text((uint8_t)(x + 6U), (uint8_t)(y + 4U), text, HAL_UI_BLACK);
    } else {
        hal_ui_rect(&box, HAL_UI_WHITE);
        hal_ui_text((uint8_t)(x + 6U), (uint8_t)(y + 4U), text, HAL_UI_WHITE);
    }
}

static void render_compose_preview_line(void)
{
    compose_reset_cursor_if_needed();

    if (g_ui_ctx.compose_length == 0U) {
        uint8_t blink = (uint8_t)((g_ui_ctx.anim.frame_count / 6U) % 2U);
        if (blink == 0U) {
            hal_ui_text(4U, 39U, "_", HAL_UI_WHITE);
        }
        return;
    }

    uint8_t start = 0U;
    const uint8_t window_chars = 21U;
    if (g_ui_ctx.compose_length > window_chars) {
        uint8_t cursor = g_ui_ctx.compose_cursor;
        if (cursor > g_ui_ctx.compose_length) {
            cursor = g_ui_ctx.compose_length;
        }
        if (cursor > (uint8_t)(window_chars / 2U)) {
            start = (uint8_t)(cursor - (window_chars / 2U));
        }
        if ((uint16_t)start + window_chars > g_ui_ctx.compose_length) {
            start = (uint8_t)(g_ui_ctx.compose_length - window_chars);
        }
    }

    uint8_t end = g_ui_ctx.compose_length;
    if ((uint16_t)start + window_chars < end) {
        end = (uint8_t)(start + window_chars);
    }

    char preview[24U];
    uint8_t out = 0U;
    uint8_t blink = (uint8_t)((g_ui_ctx.anim.frame_count / 6U) % 2U);
    for (uint8_t i = start; i < end && out < (uint8_t)(sizeof(preview) - 1U); i++) {
        if (i == g_ui_ctx.compose_cursor && blink == 0U) {
            preview[out++] = '_';
        }
        char ch = g_ui_ctx.compose_buffer[i];
        preview[out++] = (ch >= 32 && ch < 127) ? ch : '?';
    }
    if (g_ui_ctx.compose_cursor == g_ui_ctx.compose_length && blink == 0U && out < (uint8_t)(sizeof(preview) - 1U)) {
        preview[out++] = '_';
    }
    preview[out] = '\0';
    hal_ui_text(4U, 39U, preview, HAL_UI_WHITE);
}

static CeePewErr_t compose_commit_selection(uint8_t choice_idx)
{
    if (choice_idx < COMPOSE_CHAR_COUNT) {
        return compose_insert_char(COMPOSE_CHARSET[choice_idx]);
    }
    if (choice_idx == COMPOSE_ACTION_DEL_IDX) {
        return compose_delete_before_cursor();
    }
    if (choice_idx == COMPOSE_ACTION_LEFT_IDX) {
        return compose_move_cursor_left();
    }
    if (choice_idx == COMPOSE_ACTION_RIGHT_IDX) {
        return compose_move_cursor_right();
    }
    return CEEPEW_ERR_BOUNDS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1b — SELF-CONTAINED 5×7 FONT
 *
 * Bypasses hal_ui_text() which has no working font data.
 * Each character: 5 bytes = 5 columns. Each byte = 1 column, 7 rows.
 * bit 0 = topmost pixel row, bit 6 = bottommost.
 * Characters encoded for ASCII 0x20 (space) through 0x7E (~).
 * ═══════════════════════════════════════════════════════════════════════════ */

const uint8_t s_font5x7[95][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, /* 20   */
    { 0x00, 0x00, 0x5F, 0x00, 0x00 }, /* 21 ! */
    { 0x00, 0x07, 0x00, 0x07, 0x00 }, /* 22 " */
    { 0x14, 0x7F, 0x14, 0x7F, 0x14 }, /* 23 # */
    { 0x24, 0x2A, 0x7F, 0x2A, 0x12 }, /* 24 $ */
    { 0x23, 0x13, 0x08, 0x64, 0x62 }, /* 25 % */
    { 0x36, 0x49, 0x55, 0x22, 0x50 }, /* 26 & */
    { 0x00, 0x05, 0x03, 0x00, 0x00 }, /* 27 ' */
    { 0x00, 0x1C, 0x22, 0x41, 0x00 }, /* 28 ( */
    { 0x00, 0x41, 0x22, 0x1C, 0x00 }, /* 29 ) */
    { 0x08, 0x2A, 0x1C, 0x2A, 0x08 }, /* 2A * */
    { 0x08, 0x08, 0x3E, 0x08, 0x08 }, /* 2B + */
    { 0x00, 0x50, 0x30, 0x00, 0x00 }, /* 2C , */
    { 0x08, 0x08, 0x08, 0x08, 0x08 }, /* 2D - */
    { 0x00, 0x60, 0x60, 0x00, 0x00 }, /* 2E . */
    { 0x20, 0x10, 0x08, 0x04, 0x02 }, /* 2F / */
    { 0x3E, 0x51, 0x49, 0x45, 0x3E }, /* 30 0 */
    { 0x00, 0x42, 0x7F, 0x40, 0x00 }, /* 31 1 */
    { 0x42, 0x61, 0x51, 0x49, 0x46 }, /* 32 2 */
    { 0x21, 0x41, 0x45, 0x4B, 0x31 }, /* 33 3 */
    { 0x18, 0x14, 0x12, 0x7F, 0x10 }, /* 34 4 */
    { 0x27, 0x45, 0x45, 0x45, 0x39 }, /* 35 5 */
    { 0x3C, 0x4A, 0x49, 0x49, 0x30 }, /* 36 6 */
    { 0x01, 0x71, 0x09, 0x05, 0x03 }, /* 37 7 */
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, /* 38 8 */
    { 0x06, 0x49, 0x49, 0x29, 0x1E }, /* 39 9 */
    { 0x00, 0x36, 0x36, 0x00, 0x00 }, /* 3A : */
    { 0x00, 0x56, 0x36, 0x00, 0x00 }, /* 3B ; */
    { 0x00, 0x08, 0x14, 0x22, 0x41 }, /* 3C < */
    { 0x14, 0x14, 0x14, 0x14, 0x14 }, /* 3D = */
    { 0x41, 0x22, 0x14, 0x08, 0x00 }, /* 3E > */
    { 0x02, 0x01, 0x51, 0x09, 0x06 }, /* 3F ? */
    { 0x32, 0x49, 0x79, 0x41, 0x3E }, /* 40 @ */
    { 0x7E, 0x11, 0x11, 0x11, 0x7E }, /* 41 A */
    { 0x7F, 0x49, 0x49, 0x49, 0x36 }, /* 42 B */
    { 0x3E, 0x41, 0x41, 0x41, 0x22 }, /* 43 C */
    { 0x7F, 0x41, 0x41, 0x22, 0x1C }, /* 44 D */
    { 0x7F, 0x49, 0x49, 0x49, 0x41 }, /* 45 E */
    { 0x7F, 0x09, 0x09, 0x01, 0x01 }, /* 46 F */
    { 0x3E, 0x41, 0x41, 0x51, 0x72 }, /* 47 G */
    { 0x7F, 0x08, 0x08, 0x08, 0x7F }, /* 48 H */
    { 0x00, 0x41, 0x7F, 0x41, 0x00 }, /* 49 I */
    { 0x20, 0x40, 0x41, 0x3F, 0x01 }, /* 4A J */
    { 0x7F, 0x08, 0x14, 0x22, 0x41 }, /* 4B K */
    { 0x7F, 0x40, 0x40, 0x40, 0x40 }, /* 4C L */
    { 0x7F, 0x02, 0x04, 0x02, 0x7F }, /* 4D M */
    { 0x7F, 0x04, 0x08, 0x10, 0x7F }, /* 4E N */
    { 0x3E, 0x41, 0x41, 0x41, 0x3E }, /* 4F O */
    { 0x7F, 0x09, 0x09, 0x09, 0x06 }, /* 50 P */
    { 0x3E, 0x41, 0x51, 0x21, 0x5E }, /* 51 Q */
    { 0x7F, 0x09, 0x19, 0x29, 0x46 }, /* 52 R */
    { 0x46, 0x49, 0x49, 0x49, 0x31 }, /* 53 S */
    { 0x01, 0x01, 0x7F, 0x01, 0x01 }, /* 54 T */
    { 0x3F, 0x40, 0x40, 0x40, 0x3F }, /* 55 U */
    { 0x1F, 0x20, 0x40, 0x20, 0x1F }, /* 56 V */
    { 0x7F, 0x20, 0x18, 0x20, 0x7F }, /* 57 W */
    { 0x63, 0x14, 0x08, 0x14, 0x63 }, /* 58 X */
    { 0x03, 0x04, 0x78, 0x04, 0x03 }, /* 59 Y */
    { 0x61, 0x51, 0x49, 0x45, 0x43 }, /* 5A Z */
    { 0x00, 0x7F, 0x41, 0x41, 0x00 }, /* 5B [ */
    { 0x02, 0x04, 0x08, 0x10, 0x20 }, /* 5C \\ */
    { 0x00, 0x41, 0x41, 0x7F, 0x00 }, /* 5D ] */
    { 0x04, 0x02, 0x01, 0x02, 0x04 }, /* 5E ^ */
    { 0x40, 0x40, 0x40, 0x40, 0x40 }, /* 5F _ */
    { 0x00, 0x01, 0x02, 0x04, 0x00 }, /* 60 ` */
    { 0x20, 0x54, 0x54, 0x54, 0x78 }, /* 61 a */
    { 0x7F, 0x48, 0x44, 0x44, 0x38 }, /* 62 b */
    { 0x38, 0x44, 0x44, 0x44, 0x20 }, /* 63 c */
    { 0x38, 0x44, 0x44, 0x48, 0x7F }, /* 64 d */
    { 0x38, 0x54, 0x54, 0x54, 0x18 }, /* 65 e */
    { 0x08, 0x7E, 0x09, 0x01, 0x02 }, /* 66 f */
    { 0x08, 0x54, 0x54, 0x54, 0x3C }, /* 67 g */
    { 0x7F, 0x08, 0x04, 0x04, 0x78 }, /* 68 h */
    { 0x00, 0x44, 0x7D, 0x40, 0x00 }, /* 69 i */
    { 0x20, 0x40, 0x44, 0x3D, 0x00 }, /* 6A j */
    { 0x7F, 0x10, 0x28, 0x44, 0x00 }, /* 6B k */
    { 0x00, 0x41, 0x7F, 0x40, 0x00 }, /* 6C l */
    { 0x7C, 0x04, 0x18, 0x04, 0x78 }, /* 6D m */
    { 0x7C, 0x08, 0x04, 0x04, 0x78 }, /* 6E n */
    { 0x38, 0x44, 0x44, 0x44, 0x38 }, /* 6F o */
    { 0x7C, 0x14, 0x14, 0x14, 0x08 }, /* 70 p */
    { 0x08, 0x14, 0x14, 0x18, 0x7C }, /* 71 q */
    { 0x7C, 0x08, 0x04, 0x04, 0x08 }, /* 72 r */
    { 0x48, 0x54, 0x54, 0x54, 0x20 }, /* 73 s */
    { 0x04, 0x3F, 0x44, 0x40, 0x20 }, /* 74 t */
    { 0x3C, 0x40, 0x40, 0x20, 0x7C }, /* 75 u */
    { 0x1C, 0x20, 0x40, 0x20, 0x1C }, /* 76 v */
    { 0x3C, 0x40, 0x30, 0x40, 0x3C }, /* 77 w */
    { 0x44, 0x28, 0x10, 0x28, 0x44 }, /* 78 x */
    { 0x0C, 0x50, 0x50, 0x50, 0x3C }, /* 79 y */
    { 0x44, 0x64, 0x54, 0x4C, 0x44 }, /* 7A z */
    { 0x00, 0x08, 0x36, 0x41, 0x00 }, /* 7B { */
    { 0x00, 0x00, 0x7F, 0x00, 0x00 }, /* 7C | */
    { 0x00, 0x41, 0x36, 0x08, 0x00 }, /* 7D } */
    { 0x08, 0x08, 0x2A, 0x1C, 0x08 }, /* 7E ~ (right-arrow) */
};

/* Left boundary clipping for right-panel text (prevents overlap into divider at x=52).
   Rotating texts will clip cleanly at this boundary. */
#define TEXT_LEFT_CLIP  53      /* minimum x pixel (divider at 52 + 1px margin) */

/* Rotating text animation speed: time divisor in milliseconds.
   Higher value = slower animation. 60ms gives smooth 10.5s cycle. */
#define SCROLL_TIME_DIVISOR  60U

/* Render one character at pixel position (x, y). Uses draw_pixel() which
 * calls hal_ui_rect_fill() — known working. Safe: clips to screen bounds.
 * Each character is 5px wide × 7px tall with 1px column gap = 6px advance. */
static void ui_draw_char(uint8_t x, uint8_t y, char c)
{
    uint8_t idx;
    if ((uint8_t)c < 32U || (uint8_t)c > 126U) {
        idx = (uint8_t)('?' - 32U);   /* substitute for out-of-range */
    } else {
        idx = (uint8_t)((uint8_t)c - 32U);
    }

    for (uint8_t col = 0U; col < 5U; col++) {
        uint8_t col_data = s_font5x7[idx][col];
        for (uint8_t row = 0U; row < 7U; row++) {
            if ((col_data >> row) & 1U) {
                uint8_t px = (uint8_t)(x + col);
                uint8_t py = (uint8_t)(y + row);
                draw_pixel(px, py);   /* draw_pixel already clips */
            }
        }
    }
}

/* Render a NUL-terminated string starting at (x, y).
 * Advances 6px per character. Stops at screen right edge. */
static void ui_draw_text(uint8_t x, uint8_t y, const char *str)
{
    if (str == NULL) { return; }
    CEEPEW_ASSERT_VOID(x < CEEPEW_OLED_WIDTH_PX);
    CEEPEW_ASSERT_VOID(y < CEEPEW_OLED_HEIGHT_PX);
    uint8_t cx = x;
    while (*str != '\0') {
        if (cx + 5U > 128U) { break; }   /* no room for another char */
        ui_draw_char(cx, y, *str);
        cx = (uint8_t)(cx + 6U);
        str++;
    }
}

/* Clipping-aware character draw for marquee effects.
 * Accepts signed x so partially off-screen glyphs do not wrap to the
 * opposite edge when cast to uint8_t.
 * Clips on both left (TEXT_LEFT_CLIP) and right (128) boundaries. */
static void ui_draw_char_at(int16_t x, uint8_t y, char c)
{
    CEEPEW_ASSERT_VOID(y < CEEPEW_OLED_HEIGHT_PX);
    uint8_t idx;
    if ((uint8_t)c < 32U || (uint8_t)c > 126U) {
        idx = (uint8_t)('?' - 32U);
    } else {
        idx = (uint8_t)((uint8_t)c - 32U);
    }

    for (uint8_t col = 0U; col < 5U; col++) {
        int16_t px = x + (int16_t)col;
        if (px < (int16_t)TEXT_LEFT_CLIP) {
            continue;
        }
        if (px >= 128) {
            break;
        }

        uint8_t col_data = s_font5x7[idx][col];
        for (uint8_t row = 0U; row < 7U; row++) {
            if ((col_data >> row) & 1U) {
                int16_t py = (int16_t)y + (int16_t)row;
                if (py >= 0 && py < 64) {
                    draw_pixel((uint8_t)px, (uint8_t)py);
                }
            }
        }
    }
}

/* Word-wrapping text renderer for status strings that would otherwise clip.
 * Breaks on spaces when possible and hard-wraps long words within max_width. */
static void ui_draw_text_wrapped(uint8_t x, uint8_t y, const char *str,
                                 uint8_t max_width, uint8_t line_height)
{
    if (str == NULL) {
        return;
    }
    CEEPEW_ASSERT_VOID(x < CEEPEW_OLED_WIDTH_PX);
    CEEPEW_ASSERT_VOID(y < CEEPEW_OLED_HEIGHT_PX);
    CEEPEW_ASSERT_VOID(max_width > 0U);
    CEEPEW_ASSERT_VOID(line_height > 0U);

    uint8_t cx = x;
    uint8_t cy = y;

    while (*str != '\0') {
        while (*str == ' ') {
            str++;
        }
        if (*str == '\0') {
            break;
        }

        uint8_t word_len = 0U;
        while (str[word_len] != '\0' && str[word_len] != ' ') {
            word_len++;
        }

        uint16_t needed_px = (uint16_t)word_len * 6U;
        uint16_t used_px = (uint16_t)(cx - x);
        if (used_px > 0U && (used_px + needed_px) > max_width) {
            cx = x;
            cy = (uint8_t)(cy + line_height);
            if ((uint16_t)cy + 7U > 64U) {
                return;
            }
        }

        for (uint8_t i = 0U; i < word_len; i++) {
            if ((uint16_t)(cx - x) + 5U > max_width) {
                cx = x;
                cy = (uint8_t)(cy + line_height);
                if ((uint16_t)cy + 7U > 64U) {
                    return;
                }
            }
            ui_draw_char(cx, cy, str[i]);
            cx = (uint8_t)(cx + 6U);
        }

        str += word_len;
        if (*str == ' ') {
            if ((uint16_t)(cx - x) + 6U > max_width) {
                cx = x;
                cy = (uint8_t)(cy + line_height);
                if ((uint16_t)cy + 7U > 64U) {
                    return;
                }
            } else {
                cx = (uint8_t)(cx + 6U);
            }
        }
    }
}

static uint8_t ui_map_pot_to_index(uint8_t pot, uint8_t option_count)
{
    if (option_count == 0U) {
        return 0U;
    }

    /* Direct bucket mapping: keep the whole pot travel active and scale the
     * number of selectable pages/options to the current screen. */
    uint8_t idx = (uint8_t)(((uint16_t)pot * option_count) / 256U);
    if (idx >= option_count) {
        idx = (uint8_t)(option_count - 1U);
    }
    return idx;
}

static void ui_format_hex_grouped(char *out,
                                  size_t out_size,
                                  const uint8_t *bytes,
                                  uint8_t byte_count,
                                  uint8_t group_bytes)
{
    CEEPEW_ASSERT_VOID(out != NULL);
    CEEPEW_ASSERT_VOID(bytes != NULL);
    CEEPEW_ASSERT_VOID(out_size > 0U);
    CEEPEW_ASSERT_VOID(group_bytes > 0U);

    size_t needed = 1U;
    if (byte_count > 0U) {
        needed = (size_t)(byte_count * 2U) + (size_t)((byte_count - 1U) / group_bytes) + 1U;
    }
    CEEPEW_ASSERT_VOID(needed <= out_size);

    size_t pos = 0U;
    for (uint8_t i = 0U; i < byte_count; i++) {
        if ((i > 0U) && ((i % group_bytes) == 0U)) {
            out[pos++] = ' ';
        }

        uint8_t byte_val = bytes[i];
        uint8_t hi = (uint8_t)((byte_val >> 4U) & 0x0FU);
        uint8_t lo = (uint8_t)(byte_val & 0x0FU);
        out[pos++] = (hi < 10U) ? (char)('0' + hi) : (char)('A' + hi - 10U);
        out[pos++] = (lo < 10U) ? (char)('0' + lo) : (char)('A' + lo - 10U);
    }

    out[pos] = '\0';
}

static void ui_draw_hex_rows(const uint8_t *bytes, uint8_t byte_count, uint8_t x, uint8_t y)
{
    if (bytes == NULL || byte_count == 0U) {
        return;
    }

    /* Grouped hex rows keep the 32-byte commitment readable while staying
     * within the 128px OLED width. Four 2-byte groups per row => 19 chars.
     * At 6px per char, 19 chars occupy 114px and fit with a small left margin. */
    const uint8_t bytes_per_row = 8U;
    const uint8_t group_bytes = 2U;
    uint8_t row_count = (uint8_t)((byte_count + bytes_per_row - 1U) / bytes_per_row);
    for (uint8_t row = 0U; row < row_count; row++) {
        char line[24U];
        uint8_t start = (uint8_t)(row * bytes_per_row);
        uint8_t end = start + bytes_per_row;
        if (end > byte_count) {
            end = byte_count;
        }
        ui_format_hex_grouped(line, sizeof(line), &bytes[start], (uint8_t)(end - start), group_bytes);
        uint8_t line_y = (uint8_t)(y + row * 8U);
        if ((uint16_t)line_y + 7U > 64U) {
            break;
        }
        ui_draw_text(x, line_y, line);
    }
}

static void diag_draw_header(const char *title, uint8_t page_num)
{
    if (title == NULL) {
        return;
    }

    /* Draw filled rectangle for header background to ensure clean display */
    HalUIRect_t header_bg = { .x = 0U, .y = 0U, .w = 128U, .h = 9U };
    hal_ui_rect_fill(&header_bg, HAL_UI_BLACK);

    /* Draw a compact title line and keep the page indicator visible. */
    char header[40U];
    (void)snprintf(header, sizeof(header), "%s  [%u/%u]",
                   title,
                   (unsigned)(page_num + 1U),
                   (unsigned)s_diag_page_count);
    ui_draw_text(0U, 1U, header);

    /* Draw separator line */
    draw_hline(0U, 9U, 128U);
}

/* Render rotating/scrolling text that moves left continuously.
 * Used for long labels like "DISCOVERING PEERS" that exceed screen width.
 * Text scrolls at 1 pixel per frame (~50ms per pixel with 50ms frame rate).
 * Wraps around seamlessly: after text exits right edge, it re-enters from left.
 * max_width: maximum pixels to write (clipping boundary on right).
 * base_scroll_px: shared scroll offset for synchronized text (if 0, calculates independently). */
static void ui_draw_rotating_text(uint8_t x, uint8_t y, const char *str,
                                    uint8_t max_width, uint32_t base_scroll_px)
{
    if (str == NULL) { return; }

    /* Calculate text length in pixels (6px per character) */
    uint16_t text_len_px = 0U;
    const char *p = str;
    for (uint8_t i = 0U; i < 128U && *p != '\0'; i++) {
        text_len_px += 6U;
        p++;
    }

    /* Cycle only on text length (not text_len_px + max_width).
     * This ensures text wraps seamlessly without a blank gap.
     * When text exits left, it immediately re-enters from right.
     * Result: text is ALWAYS at least partially visible on screen. */
    uint16_t cycle_px = text_len_px;
    if (cycle_px == 0U) { return; }

    /* Use provided scroll offset, or calculate independently if 0 */
    uint32_t scroll_px;
    if (base_scroll_px == 0U) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        scroll_px = (now_ms / 50U) % (uint32_t)cycle_px;
    } else {
        scroll_px = base_scroll_px % (uint32_t)cycle_px;
    }
    int16_t text_x = (int16_t)x - (int16_t)scroll_px;

    /* Render each character, clipping to [x .. x+max_width) */
    const char *c = str;
    uint8_t char_idx = 0U;
    while (*c != '\0' && char_idx < 128U) {
        int16_t char_x = text_x + (int16_t)(char_idx * 6U);
        int16_t char_x_end = char_x + 5;

        /* Draw if any part of character is visible within [x .. x+max_width) */
        if (char_x_end >= (int16_t)x && char_x < (int16_t)(x + max_width)) {
            ui_draw_char_at(char_x, y, *c);
        }

        char_idx++;
        c++;
    }
}

/* ── Redirect broken HAL text/char/circle to self-contained implementations ──
 * hal_ui_rect_fill() works fine so we keep all geometry calls unchanged.
 * #undef first to avoid "macro redefinition" warnings if hal_ui.h declared them. */
#undef  hal_ui_text
#define hal_ui_text(x, y, s, colour)    ui_draw_text((uint8_t)(x), (uint8_t)(y), (s))

#undef  hal_ui_char
#define hal_ui_char(x, y, c, colour)    ui_draw_char((uint8_t)(x), (uint8_t)(y), (char)(c))

#undef  hal_ui_circle
#define hal_ui_circle(x, y, r, colour)  draw_circle((int16_t)(x), (int16_t)(y), (uint8_t)(r))

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 2 — DATA TABLES & CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Radar display constants for discovery visualization.
   RDR_CX, RDR_CY = center of 64×64 radar area on screen (now perfectly centered)
   RDR_R1, RDR_R2, RDR_R3 = concentric ring radii */
#define RDR_CX 26     /* X-center: 52/2 = 26 (2px margins on each side) */
#define RDR_CY 32     /* Y-center: 64/2 = 32 (perfect vertical centering) */
#define RDR_R1 8U
#define RDR_R2 16U
#define RDR_R3 24U

/* 16-step radar sweep endpoints, r=24 from centre (26,32).
 * Angles: 0°=E, stepping 22.5° clockwise.
 * Computed: ex = 26 + round(24*cos(θ)), ey = 32 + round(24*sin(θ))  */
typedef struct { int16_t ex; int16_t ey; } SweepPt_t;

static const SweepPt_t SWEEP16[16U] = {
    { 50, 32 }, /* 0  E    */
    { 48, 41 }, /* 1  ESE  */
    { 43, 49 }, /* 2  SE   */
    { 35, 54 }, /* 3  SSE  */
    { 26, 56 }, /* 4  S    */
    { 17, 54 }, /* 5  SSW  */
    {  9, 49 }, /* 6  SW   */
    {  4, 41 }, /* 7  WSW  */
    {  2, 32 }, /* 8  W    */
    {  4, 23 }, /* 9  WNW  */
    {  9, 15 }, /* 10 NW   */
    { 17, 10 }, /* 11 NNW  */
    { 26,  8 }, /* 12 N    */
    { 35, 10 }, /* 13 NNE  */
    { 43, 15 }, /* 14 NE   */
    { 48, 23 }, /* 15 ENE  */
};

/* Star particle origins for boot animation.
 * Particles travel from these positions toward screen centre (64, 32). */
typedef struct { uint8_t sx; uint8_t sy; } StarDef_t;

static const StarDef_t BOOT_STARS[12U] = {
    {   0,  0 }, {  32,  0 }, {  64,  0 }, {  96,  0 }, { 127,  0 },
    {   0, 32 },                                           { 127, 32 },
    {   0, 63 }, {  32, 63 }, {  64, 63 }, {  96, 63 }, { 127, 63 },
};

/* Hex character set for key-derivation matrix rain */
static const char MATRIX_CHARS[17U] = "0123456789ABCDEF";
#define MATRIX_CHARSET_LEN 16U

/* 10 column x-positions for matrix rain (character left edge) */
static const uint8_t MATRIX_COL_X[10U] = { 1,14,27,40,53,66,79,92,105,118 };

/* ────────────────────────────────────────────────────────────────────────── */
/* HELPER: draw a 1-pixel rectangle outline                                  */
/*                                                                            */
/* Design note: hal_ui_rect_fill() is the only drawing primitive available.  */
/* We decompose an outline into four 1-pixel-thick filled rectangles.        */
/* This is O(4) calls, each bounded by the compile-time screen dimensions.   */
/* ────────────────────────────────────────────────────────────────────────── */
static void draw_rect_outline(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    /* Caller must guarantee w >= 2 and h >= 2 */
    CEEPEW_ASSERT_VOID(x < CEEPEW_OLED_WIDTH_PX);
    CEEPEW_ASSERT_VOID(y < CEEPEW_OLED_HEIGHT_PX);
    CEEPEW_ASSERT_VOID((uint16_t)x + (uint16_t)w <= CEEPEW_OLED_WIDTH_PX);
    CEEPEW_ASSERT_VOID((uint16_t)y + (uint16_t)h <= CEEPEW_OLED_HEIGHT_PX);
    if (w < 2U || h < 2U) { return; }

    HalUIRect_t r;

    /* Top edge */
    r.x = x; r.y = y; r.w = w; r.h = 1U;
    hal_ui_rect_fill(&r, HAL_UI_WHITE);

    /* Bottom edge */
    r.y = (uint8_t)(y + h - 1U);
    hal_ui_rect_fill(&r, HAL_UI_WHITE);

    /* Left edge */
    r.y = (uint8_t)(y + 1U); r.w = 1U; r.h = (uint8_t)(h - 2U);
    hal_ui_rect_fill(&r, HAL_UI_WHITE);

    /* Right edge */
    r.x = (uint8_t)(x + w - 1U);
    hal_ui_rect_fill(&r, HAL_UI_WHITE);
}

/* ────────────────────────────────────────────────────────────────────────── */
/* HELPER: draw a bounded progress bar                                        */
/*                                                                            */
/* FIX A — FILL DIRECTION: inner fill rect starts at x+1 and grows RIGHT.   */
/* FIX A — LABEL POSITION: percent string placed at x + w + 4 (RIGHT side). */
/*                                                                            */
/* Layout (example w=96):                                                     */
/*   [============================          ] 47%                             */
/*    ^bar_x+1                    ^fill_w    ^bar_x + w + 4                  */
/* ────────────────────────────────────────────────────────────────────────── */
static __attribute__((unused)) void draw_progress_bar(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t pct)
{
    if (pct > 100U) { pct = 100U; }
    if (w < 4U || h < 3U) { return; }   /* minimum usable size */

    /* Outline rectangle */
    draw_rect_outline(x, y, w, h);

    /* Inner fill: LEFT → RIGHT.
     * inner_w = (inner_area_width * pct) / 100
     * inner_area_width = w - 2  (1px border on each side)
     * FIX: fill rect starts at x+1, NOT at x + inner_area - fill_w            */
    uint8_t inner_w = (uint8_t)((uint16_t)(w - 2U) * (uint16_t)pct / 100U);
    if (inner_w > 0U) {
        HalUIRect_t fill = {
            .x = (uint8_t)(x + 1U),          /* always left-anchored */
            .y = (uint8_t)(y + 1U),
            .w = inner_w,
            .h = (uint8_t)(h - 2U)
        };
        hal_ui_rect_fill(&fill, HAL_UI_WHITE);
    }

    /* Percent label on the RIGHT of the bar, not the left.
     * FIX: x position = x + w + 4, not x = 0.                               */
    char pct_str[6U];
    (void)snprintf(pct_str, sizeof(pct_str), "%3u%%", (unsigned int)pct);
    hal_ui_text((uint8_t)(x + w + 4U), y, pct_str, HAL_UI_WHITE);
}

/* ────────────────────────────────────────────────────────────────────────── */
/* HELPER: safe scrolling text (wraps a static string into a 22-char window) */
/* ────────────────────────────────────────────────────────────────────────── */
#define SCROLL_BUF_LEN  22U
static __attribute__((unused)) void draw_scroll_text(uint8_t x, uint8_t y,
                              const char *src, uint8_t src_len,
                              uint32_t now_ms, uint32_t step_ms)
{
    if (src == NULL || src_len == 0U) { return; }
    char buf[SCROLL_BUF_LEN + 1U];
    uint8_t start = (uint8_t)((now_ms / step_ms) % (uint32_t)src_len);

    /* loop bound: SCROLL_BUF_LEN (compile-time constant) */
    for (uint8_t i = 0U; i < SCROLL_BUF_LEN; i++) {
        buf[i] = src[(start + i) % src_len];
    }
    buf[SCROLL_BUF_LEN] = '\0';
    hal_ui_text(x, y, buf, HAL_UI_WHITE);
}

static CeePewErr_t render_boot_anim(void)
{
    hal_ui_clear();

    uint32_t f = g_ui_ctx.anim.frame_count;

    /* ── Phase 1 (f 0–29): Star particles converge ── */
    if (f < 60U) {
       uint32_t progress = (f < 30U) ? f : 30U;  /* clamp at 30 */
       for (uint8_t i = 0U; i < 12U; i++) {
           int16_t px = (int16_t)BOOT_STARS[i].sx
                      + (int16_t)((((int32_t)64 - BOOT_STARS[i].sx) * (int32_t)progress) / 30);
           int16_t py = (int16_t)BOOT_STARS[i].sy
                      + (int16_t)((((int32_t)32 - BOOT_STARS[i].sy) * (int32_t)progress) / 30);
           if (f < 30U) {
               /* Moving: draw as 2×2 pixel */
               draw_pixel((uint8_t)px, (uint8_t)py);
               if (px + 1 < 128 && py + 1 < 64) {
                   draw_pixel((uint8_t)(px + 1), (uint8_t)(py + 1));
               }
           } else {
               /* Arrived at centre — draw as single dot (will be covered by logo) */
               draw_pixel((uint8_t)px, (uint8_t)py);
           }
       }
    }

    /* ── Phase 2 (f 30–59): Logo letter-by-letter, each drops from top ── */
    if (f >= 30U) {
       const char   letters[7U]   = { 'C','E','E','-','P','E','W' };
       const uint8_t lx[7U]       = { 43U,50U,57U,64U,71U,78U,85U };
       const uint8_t FINAL_Y      = 12U;

       for (uint8_t i = 0U; i < 7U; i++) {
           uint32_t letter_start = 30U + (uint32_t)i * 4U;
           if (f < letter_start) { break; }

           uint32_t drop_f = f - letter_start;
           uint8_t  y_pos  = (drop_f < 6U)
                           ? (uint8_t)(drop_f * 3U)     /* drops 3 px/frame */
                           : FINAL_Y;
           if (y_pos > FINAL_Y) { y_pos = FINAL_Y; }

           char tmp[2U] = { letters[i], '\0' };
           hal_ui_text(lx[i], y_pos, tmp, HAL_UI_WHITE);
       }

       /* Underline grows from left under the logo — match logo width exactly */
       if (f >= 58U) {
           uint32_t ul_f = f - 58U;
           uint8_t ul_x = lx[0];
           uint8_t ul_final_w = (uint8_t)((lx[6] + 5U) - lx[0]); /* char width 5 */
           uint8_t ul_w = (ul_f < 20U) ? (uint8_t)(((uint32_t)ul_f * (uint32_t)ul_final_w) / 20U) : ul_final_w;
           if (ul_w > 0U) { draw_hline(ul_x, 20U, ul_w); }
       }
    }

    /* ── Phase 3 (f 60–89): Chunky loading bar fills ── */
    if (f >= 60U) {
       /* Bar border (moved up to increase gap below with frame) */
       HalUIRect_t border = { .x = 14U, .y = 42U, .w = 100U, .h = 9U };
       hal_ui_rect(&border, HAL_UI_WHITE);

       /* Chunky fill — 4-px wide segments with 1-px gaps */
       uint32_t elapsed = f - 60U;
       uint8_t  seg_count = (elapsed < 30U) ? (uint8_t)((elapsed * 12U) / 30U) : 12U;
       for (uint8_t s = 0U; s < seg_count; s++) {
           uint8_t seg_x = (uint8_t)(16U + s * 8U);
           /* Safety clamp: prevent segment from extending beyond 120px boundary */
           if (seg_x + 7U > 120U) break;
           HalUIRect_t seg = { .x = seg_x, .y = (uint8_t)(border.y + 2U), .w = 7U, .h = (uint8_t)(border.h - 4U) };
           hal_ui_rect_fill(&seg, HAL_UI_WHITE);
       }

       /* Percentage counter — placed below bar with clear separation */
       uint8_t pct = (seg_count * 100U) / 12U;
       char pct_str[5U];
       (void)snprintf(pct_str, sizeof(pct_str), "%3u%%", (unsigned int)pct);
       hal_ui_text(50U, 54U, pct_str, HAL_UI_WHITE);
    }

    /* ── Phase 4 (f 90–109): Border frame draws in from corners ── */
    if (f >= 90U) {
       uint32_t bf = f - 90U;
       uint8_t arm = (bf < 20U) ? (uint8_t)(bf * 3U) : 60U;
       if (arm > 60U) { arm = 60U; }

       /* Top-left → right and down */
       draw_hline(0U, 0U, arm);
       draw_vline(0U, 0U, (uint8_t)(arm / 2U));
       /* Top-right → left and down */
       if (arm <= 128U) {
           draw_hline((uint8_t)(128U - arm), 0U, arm);
       }
       draw_vline(127U, 0U, (uint8_t)(arm / 2U));
       /* Bottom-left → right and up */
       draw_hline(0U, 63U, arm);
       if (arm / 2U <= 63U) {
           draw_vline(0U, (uint8_t)(64U - arm / 2U), (uint8_t)(arm / 2U));
       }
       /* Bottom-right → left and up */
       if (arm <= 128U) {
           draw_hline((uint8_t)(128U - arm), 63U, arm);
       }
       if (arm / 2U <= 63U) {
           draw_vline(127U, (uint8_t)(64U - arm / 2U), (uint8_t)(arm / 2U));
       }

       /* Subtitle fades in (moved down to avoid overlap with logo) */
       /* Render at consistent position (16, 30) throughout phases 4 and 5 */
       if (bf >= 10U) {
           hal_ui_text(16U, 28U, "SECURE MESSENGER", HAL_UI_WHITE);
       }
    }

    /* ── Phase 5 (f 110–129): READY pulses, tick-mark flash ── */
    if (f >= 110U) {
       uint8_t blink = (uint8_t)((f / 5U) % 10U);
       /* Text continues from phase 4 (already rendered at 16,30) */

       /* Full border now solid */
       draw_hline(0U, 0U, 128U);
       draw_hline(0U, 63U, 128U);
       draw_vline(0U, 0U, 64U);
       draw_vline(127U, 0U, 64U);


       /* Corner tick marks flash opposite phase */
       if (blink >= 5U) {
           draw_hline(4U,  3U, 8U);   draw_hline(116U,  3U, 8U);
           draw_hline(4U, 60U, 8U);   draw_hline(116U, 60U, 8U);
           draw_vline(3U,  4U, 8U);   draw_vline(124U,  4U, 8U);
           draw_vline(3U, 52U, 8U);   draw_vline(124U, 52U, 8U);
       }
    }

    /* ── Phase 6 (f >= 130): Transition ── */
    if (f >= 130U) {
       g_ui_ctx.anim.frame_count = 0U;
       (void)ui_manager_transition_to(UI_STATE_DISCOVERY);
       g_ui_ctx.transition_ready = true;
       return CEEPEW_OK;
    }

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

static void draw_radar_static(void)
{
    /* Three concentric circles */
    draw_circle(RDR_CX, RDR_CY, RDR_R1);
    draw_circle(RDR_CX, RDR_CY, RDR_R2);
    draw_circle(RDR_CX, RDR_CY, RDR_R3);

    /* Crosshairs clipped to outer ring */
    draw_hline((uint8_t)(RDR_CX - RDR_R3), RDR_CY, (uint8_t)(RDR_R3 * 2U + 1U));
    draw_vline(RDR_CX, (uint8_t)(RDR_CY - RDR_R3), (uint8_t)(RDR_R3 * 2U + 1U));

    /* Centre dot (3×3) */
    HalUIRect_t cdot = { .x = RDR_CX - 1U, .y = RDR_CY - 1U, .w = 3U, .h = 3U };
    hal_ui_rect_fill(&cdot, HAL_UI_WHITE);

    /* Panel divider (now at x=52: perfectly centers radar with 2px margins on each side) */
    draw_vline(52U, 0U, 64U);
}

static void draw_sweep_arm(uint8_t pos)
{
    /* Restore the older, longer-lived sweep tail so the radar reads cleanly. */
    uint8_t t4 = (pos + 12U) % 16U;
    draw_radial_segment(RDR_CX, RDR_CY,
                        SWEEP16[t4].ex, SWEEP16[t4].ey,
                        RDR_R3 - 4U, RDR_R3);

    uint8_t t3 = (pos + 13U) % 16U;
    draw_radial_segment(RDR_CX, RDR_CY,
                        SWEEP16[t3].ex, SWEEP16[t3].ey,
                        RDR_R2 + 2U, RDR_R3);

    uint8_t t2 = (pos + 14U) % 16U;
    draw_radial_segment(RDR_CX, RDR_CY,
                        SWEEP16[t2].ex, SWEEP16[t2].ey,
                        RDR_R1 + 4U, RDR_R3);

    uint8_t t1 = (pos + 15U) % 16U;
    draw_radial_segment(RDR_CX, RDR_CY,
                        SWEEP16[t1].ex, SWEEP16[t1].ey,
                        4U, RDR_R3);

    /* Active arm: full bright line from centre to edge */
    draw_line(RDR_CX, RDR_CY, SWEEP16[pos].ex, SWEEP16[pos].ey);

    /* Small 2×2 cap at arm tip for better visibility (reduced from 5×5) */
    HalUIRect_t cap = {
        .x = (uint8_t)((uint8_t)SWEEP16[pos].ex - 1U),
        .y = (uint8_t)((uint8_t)SWEEP16[pos].ey - 1U),
        .w = 2U, .h = 2U
    };
    hal_ui_rect_fill(&cap, HAL_UI_WHITE);

    /* Highlighted centre dot (5×5) for sweep origin reference */
    HalUIRect_t centre = {
        .x = (uint8_t)(RDR_CX - 2U),
        .y = (uint8_t)(RDR_CY - 2U),
        .w = 5U, .h = 5U
    };
    hal_ui_rect_fill(&centre, HAL_UI_WHITE);
}

static CeePewErr_t ui_restart_discovery_from_pairing(void)
{
    CEEPEW_ASSERT(s_ui_manager_initialised, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(g_ui_ctx.current_state == UI_STATE_PAIRING ||
                  g_ui_ctx.current_state == UI_STATE_COUNTDOWN ||
                  g_ui_ctx.current_state == UI_STATE_CODE_ENTRY ||
                  g_ui_ctx.current_state == UI_STATE_PAIRING_FAILED ||
                  g_ui_ctx.current_state == UI_STATE_PAIRING_SUCCESS,
                  CEEPEW_ERR_PARAM);
    CeePewErr_t err = transport_ble_restart_discovery_session();
    if (err != CEEPEW_OK) {
        return err;
    }

    g_ui_ctx.pairing_start_ms = 0U;
    g_ui_ctx.countdown_start_ms = 0U;
    g_ui_ctx.pairing_result_start_ms = 0U;
    g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_NONE;
    g_ui_ctx.button_press_start_ms = 0U;
    g_ui_ctx.anim.frame_count = 0U;
    g_ui_ctx.transition_ready = true;
    return CEEPEW_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1c — BLE VISUAL FEEDBACK BRIDGE
 *
 * Drives the RGB LED from the BLE transport's pairing phase (or from the
 * supervisor recovery flag) so the user gets fine-grained progress
 * indication while the pairing countdown runs. Overrides the static
 * "amber pulse" mapping used by task_session_sync_visual_state() only
 * while the UI is on UI_STATE_PAIRING.
 *
 * Throttling: the LED is only reprogrammed when the desired pattern
 * actually changes — keeps I2C/LEDC traffic down and avoids stomping the
 * PWM timer mid-cycle.
 * ═══════════════════════════════════════════════════════════════════════════ */
void task_ui_update_visual_feedback(void)
{
    CEEPEW_ASSERT_VOID(s_ui_manager_initialised);

    PairingPhase_t phase = transport_ble_get_phase();
    bool recovering = transport_ble_is_recovering();

    RgbPattern_t target = recovering
                          ? RGB_YELLOW_RED_BLINK
                          : transport_ble_phase_to_rgb(phase);

    static RgbPattern_t s_last_feedback_pattern = RGB_PATTERN_COUNT;
    static bool         s_last_feedback_recovering = false;

    if (target == s_last_feedback_pattern &&
        recovering == s_last_feedback_recovering) {
        return;
    }
    s_last_feedback_pattern = target;
    s_last_feedback_recovering = recovering;

    (void)rgb_set_pattern(target);
}

/* Peer blip renderer.
 * The blip becomes more persistent and exhibits a slow, periodic drift in
 * displayed location so transient scan gaps don't make it disappear instantly
 * and to increase visual salience during discovery. */
static void draw_peer_blip(uint8_t bx, uint8_t by,
                           uint8_t sweep_pos, uint8_t blip_idx,
                           uint8_t pulse_phase, uint32_t age_ms)
{
    /* Extend lifetime so transient scan gaps do not immediately hide the blip */
    if (age_ms >= 25000U) {
        return;
    }

    /* Always draw a visible base marker (2×2) so small OLEDs show it reliably */
    HalUIRect_t base = {
        .x = (uint8_t)((bx > 0U) ? (bx - 1U) : 0U),
        .y = (uint8_t)((by > 0U) ? (by - 1U) : 0U),
        .w = 2U,
        .h = 2U
    };
    hal_ui_rect_fill(&base, HAL_UI_WHITE);

    /* Small ring so the blip stays readable on the 128x64 radar. */
    if (age_ms < 25000U) {
        draw_circle((int16_t)bx, (int16_t)by, 3U);
        if (pulse_phase == 0U || age_ms < 800U) {
            draw_circle((int16_t)bx, (int16_t)by, 5U);
        }
    }

    /* Compute a slow time-based offset so the blip drifts visibly over time.
     * This only affects the displayed index; the actual peer coordinates
     * (bx,by) remain the source of the marker. The offset updates every 2s. */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    uint8_t time_offset = (uint8_t)((now_ms / 2000U) % 16U);
    uint8_t display_idx = (uint8_t)((blip_idx + time_offset) % 16U);

    uint8_t forward = (uint8_t)((display_idx + 16U - sweep_pos) % 16U);
    uint8_t reverse = (uint8_t)((sweep_pos + 16U - display_idx) % 16U);
    uint8_t dist = (forward < reverse) ? forward : reverse;

    /* When sweep passes near blip (dist <= 1) show an expanded visual pulse */
    if (dist <= 1U) {
        HalUIRect_t blip = {
            .x = (uint8_t)((bx > 1U) ? (bx - 1U) : 0U),
            .y = (uint8_t)((by > 1U) ? (by - 1U) : 0U),
            .w = 5U,
            .h = 5U
        };
        hal_ui_rect_fill(&blip, HAL_UI_WHITE);
        /* Slight radial highlight for very fresh sightings */
        if (age_ms < 800U && pulse_phase == 0U) {
            draw_circle((int16_t)bx, (int16_t)by, 5U);
        }
    } else if (dist <= 3U) {
        /* Keep a slightly larger base for near-alignment so the blip remains
         * perceptible even when the sweep is nearby. */
        HalUIRect_t blip = {
            .x = (uint8_t)((bx > 0U) ? (bx - 1U) : 0U),
            .y = (uint8_t)((by > 0U) ? (by - 1U) : 0U),
            .w = 3U,
            .h = 3U
        };
        hal_ui_rect_fill(&blip, HAL_UI_WHITE);
    } else {
        /* Stable base marker already drawn; nothing more to do. */
    }
}

static void draw_rssi_bars(uint8_t x, uint8_t y, uint8_t bars, uint8_t pulse_phase)
{
    /* compact layout: 4px spacing, smaller heights to free vertical space */
    for (uint8_t b = 0U; b < 5U; b++) {
        uint8_t bx  = (uint8_t)(x + b * 4U);
        uint8_t bh  = (uint8_t)(1U + b); /* heights 1..5 */
        uint8_t by  = (uint8_t)(y + 6U - bh);
        HalUIRect_t bar = { .x = bx, .y = by, .w = 3U, .h = bh };
        if (b < bars) {
            hal_ui_rect_fill(&bar, HAL_UI_WHITE);
            if (pulse_phase == 0U && b == (bars - 1U) && by > 0U) {
                draw_pixel((uint8_t)(bx + 1U), (uint8_t)(by - 1U));
            }
        } else {
            hal_ui_rect(&bar, HAL_UI_WHITE);
        }
    }
}

static CeePewErr_t render_discovery(void)
{
    CEEPEW_ASSERT(s_ui_manager_initialised, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(g_ui_ctx.current_state == UI_STATE_DISCOVERY, CEEPEW_ERR_PARAM);

    hal_ui_clear();

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);

    /* ── Layout grid: consistent 9px line height (7px text + 2px gap) ── */
    /* Divider now at x=52 (centered radar with 2px margins); right panel at x=55 */
    #define RPANEL_X        55U     /* Right panel start (divider at 52 + 3px gap) */
    #define RPANEL_WIDTH    73U     /* 128 - 55 = 73px available (expanded from 67) */
    #define LINE_HEIGHT     9U      /* 7px glyph + 2px gap */
    #define Y_TITLE         1U      /* Rotating title row */
    #define Y_PEER_ID       10U     /* Peer info section */
    #define Y_RSSI_BARS     18U     /* RSSI signal bars */
    #define Y_RSSI_DBM      27U     /* dBm value */
    #define Y_PAIR_BTN      36U     /* PAIR button */
    #define Y_HINT          45U     /* Pairing hint text */
    #define Y_BLE_STATE     45U     /* BLE: XXXX state */
    #define Y_STATUS        54U     /* Status line (bottom row) */

    /* ── Left panel: Radar ── */
    draw_radar_static();

    uint8_t sweep_pos = (uint8_t)((now_ms / 150U) % 16U);
    uint8_t pulse_phase = (uint8_t)((now_ms / 300U) % 4U);
    draw_sweep_arm(sweep_pos);

    /* ── Right panel: header + status ── */
    /* Use a shared raw scroll counter; ui_draw_rotating_text() wraps per string
     * length so each label loops seamlessly without a hardcoded cycle. */
    uint32_t shared_scroll_px = (g_ui_ctx.anim.frame_count / 3U);

    /* Fixed title to keep header stable */
    hal_ui_text(RPANEL_X, Y_TITLE, "DISCOVERING PEERS", HAL_UI_WHITE);

    /* ── Peer discovery feedback ── */
    const BlePeerRecord_t *peer = transport_ble_get_peer();
    const bool peer_visible = (peer != NULL);

    if (peer_visible) {
       /* Peer found: show MAC, RSSI, pair prompt */
       char peer_id[10U];
       (void)snprintf(peer_id, sizeof(peer_id), "ID:%02X%02X%02X",
                      peer->peer_mac[3], peer->peer_mac[4],
                      peer->peer_mac[5]);
       hal_ui_text(RPANEL_X, Y_PEER_ID, peer_id, HAL_UI_WHITE);

       int16_t rssi_smooth = (int16_t)(g_ble_ctx.peer_rssi_smooth_x8 / 8);
       if (rssi_smooth < -90) { rssi_smooth = -90; }
       if (rssi_smooth > -30) { rssi_smooth = -30; }

       uint8_t bars = rssi_to_bars((int8_t)rssi_smooth);
       draw_rssi_bars(RPANEL_X, Y_RSSI_BARS, bars, pulse_phase);

       char rssi_str[10U];
       (void)snprintf(rssi_str, sizeof(rssi_str), "%ddBm", (int)rssi_smooth);
       hal_ui_text(RPANEL_X, Y_RSSI_DBM, rssi_str, HAL_UI_WHITE);

       /* Prompt user to press the button to begin pairing (only if not already confirmed) */
       if (!g_ble_ctx.commitment_verified && !transport_ble_handoff_ready()) {
           hal_ui_text(RPANEL_X, Y_PAIR_BTN, "BTN:PAIR", HAL_UI_WHITE);
           hal_ui_text(RPANEL_X, Y_HINT, "press btn", HAL_UI_WHITE);
       }

       /* Blip rendering — only visible while the peer is fresh */
       uint8_t blip_r = (uint8_t)(
           (((int16_t)(-rssi_smooth - 30) * (int16_t)(RDR_R3 - 3U)) / 60) + 2U);

       uint8_t base_idx = (uint8_t)(peer->peer_mac[5] % 16U);
       uint8_t slow_drift = (uint8_t)((g_ble_ctx.scan_hit_count / 4U) % 3U);
       int16_t rssi_delta = (int16_t)((int16_t)g_ble_ctx.peer_rssi - rssi_smooth);
       int8_t fast_wobble = (rssi_delta > 5) ? 1 : (rssi_delta < -5) ? -1 : 0;
       uint8_t blip_idx = (uint8_t)(
           ((int16_t)base_idx + (int16_t)slow_drift + (int16_t)fast_wobble + 32U) % 16U);

       int16_t ex = SWEEP16[blip_idx].ex;
       int16_t ey = SWEEP16[blip_idx].ey;
       int16_t dx = ex - RDR_CX;
       int16_t dy = ey - RDR_CY;
       int16_t bx = (int16_t)RDR_CX + (int16_t)(((int32_t)dx * blip_r) / (int32_t)RDR_R3);
       int16_t by = (int16_t)RDR_CY + (int16_t)(((int32_t)dy * blip_r) / (int32_t)RDR_R3);

       /* Blip bounds check: stay within left panel (x < 52 for divider at 52) */
       if (bx >= 0 && bx < 52 && by >= 0 && by < 64) {
           /* [FIX-1] Use thread-safe helper to get accumulated connection age
            * (mutex-protected, safe from concurrent transport updates) */
           uint32_t age_ms = transport_ble_get_peer_age_ms();
           draw_peer_blip((uint8_t)bx, (uint8_t)by, sweep_pos, blip_idx, pulse_phase, age_ms);
       }
    } else {
       /* Peer not yet found: show scanning prompt (synchronized with title above) */
       ui_draw_rotating_text(RPANEL_X, Y_PEER_ID, "Awaiting peer...", RPANEL_WIDTH, shared_scroll_px);
    }

    /* ── BLE state indicator (only when peer NOT discovered to avoid clutter) ── */
    if (!peer_visible) {
       const char *ble_state_value = "IDLE";
       switch (transport_ble_get_state()) {
           case BLE_IDLE:        ble_state_value = "IDLE"; break;
           case BLE_ADVERTISING: ble_state_value = "ADV";  break;
           case BLE_SCANNING:    ble_state_value = "SCAN"; break;
           case BLE_ADVERTISING_AND_SCANNING: ble_state_value = "ADV & SCAN"; break;
           case BLE_CONNECTED:   ble_state_value = "CON";  break;  /* Shortened from CONN */
           case BLE_PAIRING:     ble_state_value = "PAIR"; break;
           case BLE_DONE:        ble_state_value = "DONE"; break;
           default:              ble_state_value = "?";    break;
       }
       /* Draw fixed "BLE: " label, then scroll the state value */
       hal_ui_text(RPANEL_X, Y_BLE_STATE, "BLE: ", HAL_UI_WHITE);
       ui_draw_rotating_text((uint8_t)(RPANEL_X + 30U), Y_BLE_STATE, ble_state_value,
                             (uint8_t)(RPANEL_WIDTH - 30U), shared_scroll_px);
    }

    /* ── Bottom status line ── */
    if (peer_visible) {
       /* [FIX-1] Use thread-safe helper to get accumulated connection age
        * (mutex-protected, safe from concurrent transport updates) */
       uint32_t age_ms = transport_ble_get_peer_age_ms();
       char status_str[28U];
       (void)snprintf(status_str, sizeof(status_str), "S:%lu H:%u A:%lu.%01lus",
                      (unsigned long)g_ble_ctx.scan_seen_count,
                      (unsigned)g_ble_ctx.scan_hit_count,
                      (unsigned long)(age_ms / 1000U),
                      (unsigned long)((age_ms % 1000U) / 100U));
       ui_draw_rotating_text(RPANEL_X, Y_STATUS, status_str, RPANEL_WIDTH, shared_scroll_px);
    } else {
       /* Scanning: show activity summary */
       if (transport_ble_get_state() == BLE_ADVERTISING ||
           transport_ble_get_state() == BLE_SCANNING ||
           transport_ble_get_state() == BLE_ADVERTISING_AND_SCANNING) {
           char adv_str[20U];
           (void)snprintf(adv_str, sizeof(adv_str), "SCAN S:%lu",
                          (unsigned long)g_ble_ctx.scan_seen_count);
           ui_draw_rotating_text(RPANEL_X, Y_STATUS, adv_str, RPANEL_WIDTH, shared_scroll_px);
       } else {
           static const char *const SCAN_STATES[4] = { "SCAN", "SCAN.", "SCAN..", "SCAN..." };
           uint8_t dot_idx = (uint8_t)((now_ms / 500U) % 4U);
           hal_ui_text(RPANEL_X, Y_STATUS, SCAN_STATES[dot_idx], HAL_UI_WHITE);
       }
    }

    return CEEPEW_OK;
}

static const char *ui_pairing_result_reason_text(uint8_t reason)
{
    switch ((PairingResultReason_t)reason) {
        case UI_PAIRING_RESULT_SUCCESS: return "PAIRING COMPLETE";
        case UI_PAIRING_RESULT_TIMED_OUT: return "PAIRING TIMED OUT";
        case UI_PAIRING_RESULT_LINK_FAIL: return "PAIRING LINK FAILED";
        case UI_PAIRING_RESULT_COMMITMENT_FAIL: return "PAIRING MISMATCH";
        case UI_PAIRING_RESULT_UNKNOWN:
        case UI_PAIRING_RESULT_NONE:
        default: return "PAIRING FAILED";
    }
}

static const char *ui_pairing_result_detail_text(uint8_t reason)
{
    switch ((PairingResultReason_t)reason) {
        case UI_PAIRING_RESULT_SUCCESS:
            return "Link established cleanly.";
        case UI_PAIRING_RESULT_TIMED_OUT:
            return "Timer expired before handshake.";
        case UI_PAIRING_RESULT_LINK_FAIL:
            return "Pairing Failed. Bring devices closer";
        case UI_PAIRING_RESULT_COMMITMENT_FAIL:
            return "Code mismatch. Verify codes match.";
        case UI_PAIRING_RESULT_UNKNOWN:
        case UI_PAIRING_RESULT_NONE:
        default:
            return "Pairing did not finish cleanly.";
    }
}

static void ui_draw_centered_text(uint8_t y, const char *text)
{
    if (text == NULL) {
        return;
    }

    uint8_t len = (uint8_t)strlen(text);
    uint16_t px = (uint16_t)len * 6U;
    uint8_t x = 0U;
    if (px < HAL_UI_WIDTH_PX) {
        x = (uint8_t)((HAL_UI_WIDTH_PX - px) / 2U);
    }

    hal_ui_text(x, y, text, HAL_UI_WHITE);
}

static CeePewErr_t render_code_entry(void)
{
    hal_ui_clear();

    uint32_t f = g_ui_ctx.anim.frame_count;

    /* Title: center the text to avoid clipping on small OLEDs */
    {
        const char *title = "ENTER PAIRING CODE";
        uint8_t tlen = (uint8_t)strlen(title);
        uint8_t tx = (uint8_t)(((uint16_t)HAL_UI_WIDTH_PX - (uint16_t)tlen * 6U) / 2U);
        hal_ui_text(tx, 2U, title, HAL_UI_WHITE);
    }
    draw_hline(0U, 12U, 128U);

    /* Four aligned cells spread across the full width. */
    const uint8_t cell_x[4U] = { 8U, 38U, 68U, 98U };
    const uint8_t cell_w     = 22U;
    const uint8_t cell_h     = 18U;
    const uint8_t cell_y     = 14U;

    for (uint8_t i = 0U; i < 4U; i++) {
        bool is_active = (g_ui_ctx.code_selected == i);

        if (is_active) {
            /* Active cell: double border blinks at ~4Hz */
            uint8_t blink = (uint8_t)((f / 8U) % 2U);
            HalUIRect_t outer = { .x = (uint8_t)(cell_x[i] - 2U), .y = (uint8_t)(cell_y - 2U),
                                  .w = (uint8_t)(cell_w + 4U), .h = (uint8_t)(cell_h + 4U) };
            if (blink == 0U) { hal_ui_rect(&outer, HAL_UI_WHITE); }
            HalUIRect_t inner = { .x = cell_x[i], .y = cell_y,
                                  .w = cell_w, .h = cell_h };
            hal_ui_rect(&inner, HAL_UI_WHITE);
        } else {
            /* Inactive cell: single border */
            HalUIRect_t box = { .x = cell_x[i], .y = cell_y,
                                .w = cell_w, .h = cell_h };
            hal_ui_rect(&box, HAL_UI_WHITE);
        }

        /* Digit/char character (large, centred in cell).
         * g_ui_ctx.code_digits stores an ASCII byte for display (0-9 or A-Z).
         * Fall back to numeric mapping if value is low. */
        char digit_str[2U];
        char ch;
        if (g_ui_ctx.code_digits[i] >= 32U) {
            ch = (char)g_ui_ctx.code_digits[i];
        } else {
            ch = (char)('0' + (g_ui_ctx.code_digits[i] % 10U));
        }
        digit_str[0] = ch; digit_str[1] = '\0';

        /* Digit blinks in active cell to show it's editable */
        bool show_digit = true;
        if (is_active) {
            show_digit = (uint8_t)((f / 6U) % 3U) != 2U; /* 2 frames on, 1 off */
        }
        if (show_digit) {
            hal_ui_text((uint8_t)(cell_x[i] + 7U), (uint8_t)(cell_y + 5U), digit_str, HAL_UI_WHITE);
        }
    }

    /* Index indicator dots below cells */
    for (uint8_t i = 0U; i < 4U; i++) {
        uint8_t dot_x = (uint8_t)(cell_x[i] + cell_w / 2U);
        if (g_ui_ctx.code_selected == i) {
            HalUIRect_t dot = { .x = (uint8_t)(dot_x - 2U), .y = 38U, .w = 5U, .h = 5U };
            hal_ui_rect_fill(&dot, HAL_UI_WHITE);
        } else if (i < g_ui_ctx.code_selected) {
            HalUIRect_t dot = { .x = (uint8_t)(dot_x - 1U), .y = 39U, .w = 3U, .h = 3U };
            hal_ui_rect_fill(&dot, HAL_UI_WHITE);
        } else {
            draw_pixel(dot_x, 40U);
        }
    }

    hal_ui_text(8U, 48U, "short press : next", HAL_UI_WHITE);
    hal_ui_text(8U, 56U, "long press : enter", HAL_UI_WHITE);

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

static CeePewErr_t render_countdown(void)
{
    hal_ui_clear();

    /* Dedicated pairing stage with an extended window for GATT setup. */
    if (session_is_active() || transport_ble_handoff_ready()) {
        g_ui_ctx.anim.frame_count = 0U;
        g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_SUCCESS;
        (void)ui_manager_transition_to(UI_STATE_PAIRING_SUCCESS);
        g_ui_ctx.transition_ready = true;
        return CEEPEW_OK;
    }

    uint32_t now  = (uint32_t)(esp_timer_get_time() / 1000LL);
    const BlePeerRecord_t *peer = transport_ble_get_peer();

    if (!g_ble_ctx.gattc_connected &&
        !g_ble_ctx.gatts_connected &&
        !g_ble_ctx.connecting &&
        peer == NULL &&
        g_ui_ctx.pairing_start_ms != 0U &&
        (now >= g_ui_ctx.pairing_start_ms) &&
        (now - g_ui_ctx.pairing_start_ms) >= CEEPEW_PAIRING_PEER_LOSS_MS) {
        g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_LINK_FAIL;
        (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
        g_ui_ctx.transition_ready = true;
        return CEEPEW_OK;
    }

    uint32_t elapsed = (now >= g_ui_ctx.pairing_start_ms)
                     ? (now - g_ui_ctx.pairing_start_ms) : 0U;
    uint32_t total_ms = 45000U;
    uint32_t rem_ms   = (elapsed < total_ms) ? (total_ms - elapsed) : 0U;

    /* Title */
    hal_ui_text(34U, 2U, "PAIRING", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    /* Show the fixed 4-digit code while the pairing window runs. */
    const uint8_t code_x[4U] = { 10U, 40U, 70U, 100U };
    const uint8_t code_y = 14U;
    const uint8_t code_w = 14U;
    const uint8_t code_h = 12U;
    for (uint8_t i = 0U; i < 4U; i++) {
        HalUIRect_t box = { .x = code_x[i], .y = code_y, .w = code_w, .h = code_h };
        hal_ui_rect(&box, HAL_UI_WHITE);
        char digit[2U] = { (char)g_ui_ctx.code_digits[i], '\0' };
        hal_ui_text((uint8_t)(code_x[i] + 4U), (uint8_t)(code_y + 2U), digit, HAL_UI_WHITE);
    }

    /* Progress bar — fills as time decreases. (moved up to avoid overflow) */
    uint32_t fill = (rem_ms * 120U) / total_ms;
    if (fill > 120U) { fill = 120U; }

    HalUIRect_t border = { .x = 4U, .y = 37U, .w = 120U, .h = 8U };
    hal_ui_rect(&border, HAL_UI_WHITE);
    if (fill > 0U) {
        HalUIRect_t bar = { .x = 6U, .y = 39U, .w = (uint8_t)fill, .h = 4U };
        hal_ui_rect_fill(&bar, HAL_UI_WHITE);
    }

    /* Bottom status line: keep waiting text anchored to Y_STATUS for consistency */
    ui_draw_text_wrapped(10U, Y_STATUS, "Waiting for peer", 108U, 8U);

    if (rem_ms == 0U) {
        g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_TIMED_OUT;
        (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
        g_ui_ctx.transition_ready = true;
        return CEEPEW_OK;
    }

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

static CeePewErr_t render_syncing(void)
{
    return render_countdown();
}

static CeePewErr_t render_pairing_success(void)
{
    hal_ui_clear();

    uint32_t f = g_ui_ctx.anim.frame_count;
    uint8_t blink = (uint8_t)((f / 6U) % 2U);

    hal_ui_text(22U, 2U, "PAIRING OK", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    HalUIRect_t ok_box = { .x = 20U, .y = 20U, .w = 88U, .h = 20U };
    hal_ui_rect(&ok_box, HAL_UI_WHITE);
    if (blink == 0U) {
        HalUIRect_t tick = { .x = 96U, .y = 24U, .w = 6U, .h = 6U };
        hal_ui_rect_fill(&tick, HAL_UI_WHITE);
    }
    hal_ui_text(30U, 26U, "Session ready", HAL_UI_WHITE);
    ui_draw_text_wrapped(10U, 34U, "Moving to key derivation", 108U, 8U);
    /* Keep the simple waiting prompt in the bottom status area for visibility */
    ui_draw_text_wrapped(22U, Y_STATUS, "Please wait...", 84U, 8U);

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

static CeePewErr_t render_pairing_failed(void)
{
    hal_ui_clear();

    const char *reason = ui_pairing_result_reason_text(g_ui_ctx.pairing_result_reason);
    const char *detail = ui_pairing_result_detail_text(g_ui_ctx.pairing_result_reason);

    hal_ui_text(16U, 2U, "PAIRING FAILED", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    ui_draw_centered_text(15U, reason);

    HalUIRect_t detail_box = { .x = 8U, .y = 22U, .w = 112U, .h = 22U };
    hal_ui_rect(&detail_box, HAL_UI_WHITE);
    ui_draw_text_wrapped(12U, 25U, detail, 102U, 8U);

    if ((g_ui_ctx.anim.frame_count / 5U) % 2U == 0U) {
        HalUIRect_t tick = { .x = 112U, .y = 50U, .w = 6U, .h = 6U };
        hal_ui_rect_fill(&tick, HAL_UI_WHITE);
    }
    /* Slightly increase spacing between detail box and footer text to improve legibility */
    draw_hline(8U, 50U, 112U);
    ui_draw_text_wrapped(12U, Y_STATUS, "System Syncing... Please wait.", 102U, 8U);

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

static CeePewErr_t render_confirm(void)
{
    hal_ui_clear();

    uint32_t f = g_ui_ctx.anim.frame_count;

    hal_ui_text(22U, 2U, "CONFIRM PAIRING?", HAL_UI_WHITE);
    draw_hline(0U, 12U, 128U);

    /* Animated concentric rings — shifted upward to avoid the bottom prompt */
    uint8_t ring_phase = (uint8_t)((f / 3U) % 24U);
    if (ring_phase > 0U && ring_phase <= 8U) {
        draw_circle(64, 24, (uint8_t)(ring_phase * 2U));
    }

    /* Lock icon — simplified: a rect (body) + arc (shackle) */
    draw_circle(64, 20, 6);                       /* shackle arc (top half) */
    HalUIRect_t lock_body = { .x = 56U, .y = 22U, .w = 16U, .h = 12U };
    hal_ui_rect_fill(&lock_body, HAL_UI_WHITE);
    /* Keyhole would go at (62,33) but we can't clear pixels, so skip */

    /* Prompt box at bottom — pulses */
    uint8_t blink = (uint8_t)((f / 8U) % 2U);
    HalUIRect_t prompt = { .x = 14U, .y = 44U, .w = 100U, .h = 20U };
    if (blink == 0U) {
        hal_ui_rect(&prompt, HAL_UI_WHITE);
        hal_ui_text(18U, 49U, "HOLD BTN TO PAIR", HAL_UI_WHITE);
        /* Small hint for DIAG users about Info Mode */
        hal_ui_text(18U, 54U, "Hold for Info", HAL_UI_WHITE);
        hal_ui_text(80U, 54U, "[DIAG]", HAL_UI_WHITE);
    } else {
        hal_ui_rect_fill(&prompt, HAL_UI_WHITE);
        /* Invert text — can't truly invert, so just skip text on filled frame */
    }

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

/* DIAG-only Info screen: displays diagnostic info and exits on short press or timeout */
static CeePewErr_t render_info(void)
{
    hal_ui_clear();
    /* Title */
    hal_ui_text(18U, 2U, "DEVICE INFO (DIAG)", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    /* MAC and firmware */
    char mac_str[24U];
    uint8_t local_mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, local_mac) == ESP_OK) {
        (void)snprintf(mac_str, sizeof(mac_str), "STA MAC: %02X:%02X:%02X", local_mac[0], local_mac[1], local_mac[2]);
        hal_ui_text(4U, 14U, mac_str, HAL_UI_WHITE);
        (void)snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X", local_mac[3], local_mac[4], local_mac[5]);
        hal_ui_text(4U, 22U, mac_str, HAL_UI_WHITE);
    } else {
        hal_ui_text(4U, 14U, "STA MAC: <err>", HAL_UI_WHITE);
    }

    hal_ui_text(4U, 30U, "FW: ceepew", HAL_UI_WHITE);

    /* Show last secure wipe timestamp/counter if available via session_fsm */
    extern uint32_t session_get_last_wipe_ms(void);
    uint32_t lw = session_get_last_wipe_ms();
    char lw_str[24U];
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (lw != 0U) {
        uint32_t age_s = (now_ms > lw) ? (uint32_t)((now_ms - lw) / 1000U) : 0U;
        (void)snprintf(lw_str, sizeof(lw_str), "Last wipe: %lus ago", (unsigned long)age_s);
        hal_ui_text(4U, 38U, lw_str, HAL_UI_WHITE);
    } else {
        hal_ui_text(4U, 38U, "Last wipe: never", HAL_UI_WHITE);
    }

    /* DIAG overhaul: build a paginated diagnostic view and let the pot select the page */
    char lines[24U][28U];
    uint8_t line_count = 0U;

    /* Basic info */
    (void)snprintf(lines[line_count++], sizeof(lines[0]), "APP: CEEPEW");

    uint64_t up_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
    (void)snprintf(lines[line_count++], sizeof(lines[0]), "Uptime:%lus", (unsigned long)(up_ms / 1000ULL));

    size_t heap_free = esp_get_free_heap_size();
    (void)snprintf(lines[line_count++], sizeof(lines[0]), "Heap:%uK", (unsigned int)(heap_free / 1024U));

    /* ADC snapshot */
    extern CeePewErr_t input_get_adc_snapshot(uint16_t *raw_out, uint16_t *smoothed_out);
    uint16_t raw = 0U, sm = 0U;
    if (input_get_adc_snapshot(&raw, &sm) == CEEPEW_OK) {
        (void)snprintf(lines[line_count++], sizeof(lines[0]), "ADC R:%u S:%u", (unsigned)raw, (unsigned)sm);
    } else {
        (void)snprintf(lines[line_count++], sizeof(lines[0]), "ADC: not ready");
    }

    /* BLE state */
    const char *bst = "BLE:?";
    switch (transport_ble_get_state()) {
        case BLE_IDLE: bst = "BLE:IDLE"; break;
        case BLE_ADVERTISING: bst = "BLE:ADV"; break;
        case BLE_SCANNING: bst = "BLE:SCAN"; break;
        case BLE_ADVERTISING_AND_SCANNING: bst = "BLE:ADV+SCN"; break;
        case BLE_CONNECTED: bst = "BLE:CONN"; break;
        case BLE_PAIRING: bst = "BLE:PAIR"; break;
        case BLE_DONE: bst = "BLE:DONE"; break;
        default: break;
    }
    (void)snprintf(lines[line_count++], sizeof(lines[0]), "%s", bst);

    const BlePeerRecord_t *peer = transport_ble_get_peer();
    if (peer != NULL) {
        (void)snprintf(lines[line_count++], sizeof(lines[0]), "Peer:%02X%02X%02X R:%ddBm",
                       peer->peer_mac[3], peer->peer_mac[4], peer->peer_mac[5], (int)g_ble_ctx.peer_rssi);
        (void)snprintf(lines[line_count++], sizeof(lines[0]), "Seen:%lu Hits:%u", (unsigned long)g_ble_ctx.scan_seen_count, (unsigned)g_ble_ctx.scan_hit_count);
    }

    /* Session info */
    uint8_t phase = session_get_phase();
    (void)snprintf(lines[line_count++], sizeof(lines[0]), "Phase:%u", (unsigned)phase);
    uint64_t sid = session_get_id();
    (void)snprintf(lines[line_count++], sizeof(lines[0]), "SessID:0x%08lX", (unsigned long)(sid & 0xFFFFFFFFUL));
    uint64_t nc = session_get_nonce_counter();
    (void)snprintf(lines[line_count++], sizeof(lines[0]), "Nonce:%lu", (unsigned long)nc);

    /* Commitment preview (first 8 bytes as hex) */
    uint8_t commit[CEEPEW_COMMITMENT_BYTES];
    if (session_get_commitment(commit) == CEEPEW_OK) {
        char chex[17U];
        for (uint8_t i = 0U; i < 8U; i++) {
            uint8_t b = commit[i];
            chex[i * 2U]     = (char)((b >> 4U) < 10U ? '0' + (b >> 4U) : 'A' + (b >> 4U) - 10U);
            chex[i * 2U + 1U] = (char)((b & 0x0FU) < 10U ? '0' + (b & 0x0FU) : 'A' + (b & 0x0FU) - 10U);
        }
        chex[16U] = '\0';
        (void)snprintf(lines[line_count++], sizeof(lines[0]), "Commit:%s", chex);
    }

    (void)snprintf(lines[line_count++], sizeof(lines[0]), "Ticks:%u", (unsigned)g_ui_ctx.anim.frame_count);

    if (line_count > 24U) { line_count = 24U; }

    /* Dynamic paging: allow pot to scroll through lines without fixed page count */
    uint8_t lines_per_page = (uint8_t)((64U - 12U) / 9U); /* available rows based on header */
    if (lines_per_page == 0U) { lines_per_page = 1U; }

    uint8_t max_offset = (line_count > lines_per_page) ? (uint8_t)(line_count - lines_per_page) : 0U;
    uint8_t pot = g_ui_ctx.user_input;
    uint8_t offset = (uint8_t)(((uint16_t)pot * (uint16_t)(max_offset + 1U)) / 256U);

    uint8_t y = 6U;
    for (uint8_t i = 0U; i < lines_per_page; i++) {
        uint8_t idx = (uint8_t)(offset + i);
        if (idx >= line_count) { break; }
        hal_ui_text(2U, (uint8_t)(y + i * 9U), lines[idx], HAL_UI_WHITE);
    }

    /* Show current top-line index and total lines as DIAG L: top/total */
    char pg_str[16U];
    (void)snprintf(pg_str, sizeof(pg_str), "DIAG L:%u/%u", (unsigned)(offset + 1U), (unsigned)line_count);
    hal_ui_text(80U, 0U, pg_str, HAL_UI_WHITE);

    uint8_t fill = (uint8_t)((uint16_t)pot * 40U / 255U);
    HalUIRect_t pbg = { .x = 4U, .y = 56U, .w = 120U, .h = 6U };
    hal_ui_rect(&pbg, HAL_UI_WHITE);
    if (fill > 0U) {
        HalUIRect_t pf = { .x = 6U, .y = 58U, .w = fill, .h = 2U };
        hal_ui_rect_fill(&pf, HAL_UI_WHITE);
    }

    return CEEPEW_OK;
}

static CeePewErr_t render_keyder_anim(void)
{
    hal_ui_clear();

    uint32_t f = g_ui_ctx.anim.frame_count;

    /* 6 character rows, y positions */
    const uint8_t ROW_Y[6U] = { 2U, 10U, 18U, 26U, 34U, 42U };
    const uint8_t NUM_ROWS  = 6U;

    /* Draw matrix rain */
    for (uint8_t col = 0U; col < 10U; col++) {
        /* Head row advances 1 row every 4 frames, offset by column */
        uint32_t head_f = f + (uint32_t)col * 6U;
        uint8_t  head_row = (uint8_t)((head_f / 4U) % (NUM_ROWS + 4U));

        for (uint8_t row = 0U; row < NUM_ROWS; row++) {
            if (row > head_row) { continue; }  /* not yet fallen */

            /* Character selection: pseudo-random from frame + col + row */
            uint8_t char_idx = (uint8_t)(
                ((uint32_t)(col * 17U) + (head_f / 2U) + (uint32_t)(row * 13U))
                % MATRIX_CHARSET_LEN
            );
            char ch[2U] = { MATRIX_CHARS[char_idx], '\0' };

            if (row == head_row) {
                /* Head character: draw with small box (bright) */
                hal_ui_text(MATRIX_COL_X[col], ROW_Y[row], ch, HAL_UI_WHITE);
                HalUIRect_t hd = { .x = MATRIX_COL_X[col] - 1U,
                                   .y = ROW_Y[row] - 1U,
                                   .w = 8U, .h = 10U };
                hal_ui_rect(&hd, HAL_UI_WHITE);
            } else {
                /* Trailing character */
                hal_ui_text(MATRIX_COL_X[col], ROW_Y[row], ch, HAL_UI_WHITE);
            }
        }
    }

    /* Horizontal scan-bar sweeps down slowly — overlaps matrix */
    uint8_t scan_y = (uint8_t)((f / 2U) % 52U);
    draw_hline(0U, scan_y, 128U);

    /* Progress bar */
    /* Progress band moved up to avoid overlapping bottom status/footer */
    HalUIRect_t pbar_bg = { .x = 0U, .y = 44U, .w = 128U, .h = 10U };
    hal_ui_rect_fill(&pbar_bg, HAL_UI_WHITE); /* white background strip */
    /* Draw an inner border to create a progress bar look */
    HalUIRect_t pbar_border = { .x = 2U, .y = 46U, .w = 124U, .h = 6U };
    hal_ui_rect(&pbar_border, HAL_UI_WHITE);

    /* Derive animation progress from frame (assume ~150 frames for full derivation) */
    uint8_t prog_w = (uint8_t)((f < 150U) ? (f * 120U / 150U) : 120U);
    if (prog_w > 0U) {
        HalUIRect_t prog = { .x = 4U, .y = 48U, .w = prog_w, .h = 4U };
        hal_ui_rect_fill(&prog, HAL_UI_WHITE);
    }

    /* Label above the progress band to avoid conflict with bottom status line */
    hal_ui_text(24U, 36U, "DERIVING KEY...", HAL_UI_WHITE);

    g_ui_ctx.anim.frame_count++;

    /* FIX: transition to cryptogram once animation completes (~3 s at 50 ms/frame) */
    if (g_ui_ctx.anim.frame_count >= 150U) {
        g_ui_ctx.anim.frame_count = 0U;
        (void)ui_manager_transition_to(UI_STATE_CRYPTOGRAM);
        g_ui_ctx.transition_ready = true;
    }

    return CEEPEW_OK;
}

static CeePewErr_t render_fingerprint(void)
{
    hal_ui_clear();

    uint32_t f = g_ui_ctx.anim.frame_count;

    hal_ui_text(4U, 2U, "PEER FINGERPRINT", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    /* Build full 32-char hex string from stored fingerprint */
    char fp_hex[33U];
    for (uint8_t i = 0U; i < 16U; i++) {
        uint8_t  b    = g_ui_ctx.fingerprint[i];
        uint8_t  h    = (b >> 4U) & 0x0FU;
        uint8_t  l    = b & 0x0FU;
        fp_hex[i * 2U]       = (h < 10U) ? (char)('0' + h) : (char)('A' + h - 10U);
        fp_hex[i * 2U + 1U]  = (l < 10U) ? (char)('0' + l) : (char)('A' + l - 10U);
    }
    fp_hex[32U] = '\0';

    /* Reveal 2 characters per frame up to full 32 */
    uint8_t reveal = (uint8_t)((f * 2U < 32U) ? f * 2U : 32U);

    /* Row 1: chars 0–15 at y=16 */
    char row1[17U];
    uint8_t r1_len = (reveal < 16U) ? reveal : 16U;
    (void)memcpy(row1, fp_hex, r1_len);
    row1[r1_len] = '\0';
    hal_ui_text(4U, 16U, row1, HAL_UI_WHITE);

    /* Cursor blink after last revealed char in row 1 */
    if (reveal < 16U) {
        uint8_t cur_x = (uint8_t)(4U + reveal * 6U);
        uint8_t cur_on = (uint8_t)((f / 4U) % 2U);
        if (cur_on == 0U) { draw_vline(cur_x, 16U, 8U); }
    }

    /* Row 2: chars 16–31 at y=26 */
    if (reveal > 16U) {
        char row2[17U];
        uint8_t r2_len = (uint8_t)(reveal - 16U);
        (void)memcpy(row2, fp_hex + 16U, r2_len);
        row2[r2_len] = '\0';
        hal_ui_text(4U, 26U, row2, HAL_UI_WHITE);

        if (reveal < 32U) {
            uint8_t cur_x = (uint8_t)(4U + (reveal - 16U) * 6U);
            uint8_t cur_on = (uint8_t)((f / 4U) % 2U);
            if (cur_on == 0U) { draw_vline(cur_x, 26U, 8U); }
        }
    }

    /* Peer MAC */
    char mac_str[18U];
    (void)snprintf(mac_str, sizeof(mac_str), "Peer: %02X%02X%02X%02X",
                   g_ui_ctx.peer_mac[2U], g_ui_ctx.peer_mac[3U],
                   g_ui_ctx.peer_mac[4U], g_ui_ctx.peer_mac[5U]);
    hal_ui_text(4U, 38U, mac_str, HAL_UI_WHITE);

    /* After full reveal: prompt stays within the right-panel width */
    if (reveal >= 32U) {
        uint8_t blink = (uint8_t)((f / 8U) % 2U);
        if (blink == 0U) {
            ui_draw_text_wrapped(8U, 52U, "Press to confirm", 112U, 8U);
        }
        /* Animated bracket markers */
        draw_hline(0U,  50U, 6U);
        draw_hline(122U,50U, 6U);
        draw_vline(0U,  50U, 14U);
        draw_vline(127U,50U, 14U);
    }

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

static CeePewErr_t render_fingerprint_confirm(void)
{
    hal_ui_clear();

    uint32_t f = g_ui_ctx.anim.frame_count;
    char mac_str[16U];
    uint8_t blink;

    hal_ui_text(22U, 2U, "VERIFY IDENTITY", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    /* Grouped fingerprint rows leave room for the peer MAC and buttons. */
    ui_draw_hex_rows(g_ui_ctx.fingerprint, 16U, 4U, 14U);

    /* Peer MAC */
    (void)snprintf(mac_str, sizeof(mac_str), "Peer: %02X%02X",
                   g_ui_ctx.peer_mac[4U], g_ui_ctx.peer_mac[5U]);
    hal_ui_text(4U, 36U, mac_str, HAL_UI_WHITE);

    /* Accept / reject buttons — animated outlines */
    blink = (uint8_t)((f / 6U) % 2U);

    /* ACCEPT button (left) */
    HalUIRect_t yes = { .x = 4U, .y = 48U, .w = 50U, .h = 14U };
    hal_ui_rect(&yes, HAL_UI_WHITE);
    hal_ui_text(12U, 52U, "D=ACCEPT", HAL_UI_WHITE);
    if (blink == 0U) {
        /* Pulsing inner highlight on accept */
        HalUIRect_t yes_hi = { .x = 6U, .y = 50U, .w = 46U, .h = 10U };
        hal_ui_rect(&yes_hi, HAL_UI_WHITE);
    }

    /* REJECT button (right) */
    HalUIRect_t no = { .x = 74U, .y = 48U, .w = 50U, .h = 14U };
    hal_ui_rect(&no, HAL_UI_WHITE);
    hal_ui_text(82U, 52U, "S=DENY", HAL_UI_WHITE);

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

/* Sprint 11: Helper function to display individual chat message bubble.
 * Renders a single message as a bubble with text and optional status indicator.
 * Positions left for RX (dir=0) and right for TX (dir=1).
 * No dynamic allocation; static buffers for text display.
 * Two CEEPEW_ASSERTs for bounds checking.
 */
CeePewErr_t ui_chat_show_bubble(uint8_t msg_idx, uint8_t y_pos, uint8_t dir)
{
    CEEPEW_ASSERT(msg_idx < CEEPEW_MAX_MESSAGES, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(dir <= 1U, CEEPEW_ERR_BOUNDS);

    /* Retrieve message from store (0=RX, 1=TX) */
    const StoredMsg_t *msg = msg_store_get(msg_idx);
    if (msg == NULL) { return CEEPEW_OK; /* Skip if not available */ }

    /* Map direction: 0=RX (left), 1=TX (right) */
    uint8_t bubble_dir = (msg->meta.dir <= 1U) ? msg->meta.dir : dir;
    uint8_t bubble_x = (bubble_dir == 0U) ? 4U : 28U;
    uint8_t bubble_w = 96U;
    uint8_t text_x = (bubble_dir == 0U) ? 8U : 32U;

    /* Draw bubble outline */
    HalUIRect_t bubble = {.x = bubble_x, .y = y_pos, .w = bubble_w, .h = 11U};
    hal_ui_rect(&bubble, HAL_UI_WHITE);

    /* Show compact metadata rather than ciphertext bytes. */
    char preview[24U];
    const char *side = (bubble_dir == 0U) ? "RX" : "TX";
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    uint32_t age_s = (now_s > msg->meta.created_at) ? (now_s - msg->meta.created_at) : 0U;
    (void)snprintf(preview, sizeof(preview), "%s %uB %lus",
                   side, (unsigned)msg->meta.payload_len, (unsigned long)age_s);
    hal_ui_text(text_x, (uint8_t)(y_pos + 2U), preview, HAL_UI_WHITE);

    /* Add status indicator (TTL countdown or delivery) */
    uint32_t ttl_remaining = (age_s < CEEPEW_MSG_TTL_S) ? (CEEPEW_MSG_TTL_S - age_s) : 0U;

    /* Draw TTL indicator as small circle at top-right of bubble */
    if (ttl_remaining > 0U) {
        uint8_t status_x = (bubble_dir == 0U) ? 96U : 120U;
        hal_ui_circle(status_x, y_pos, 1U, HAL_UI_WHITE);
    }

    return CEEPEW_OK;
}

static CeePewErr_t render_chat_thread(void)
{
    hal_ui_clear();

    hal_ui_text(20U, 1U, "SECURE CHAT", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    uint8_t count = msg_store_count();
    char badge[16U];
    (void)snprintf(badge, sizeof(badge), "M%u", (unsigned)count);
    hal_ui_text(102U, 1U, badge, HAL_UI_WHITE);

    if (count == 0U) {
        hal_ui_text(22U, 28U, "No messages yet", HAL_UI_WHITE);
        hal_ui_text(30U, 48U, "BTN=menu", HAL_UI_WHITE);
        return CEEPEW_OK;
    }

    uint8_t shown = (count < 3U) ? count : 3U;
    uint8_t start = (count > shown) ? (uint8_t)(count - shown) : 0U;

    for (uint8_t i = 0U; i < shown; i++) {
        uint8_t idx = (uint8_t)(start + i);
        const StoredMsg_t *msg = msg_store_get(idx);
        if (msg == NULL) {
            continue;
        }
        uint8_t y = (uint8_t)(14U + i * 14U);
        (void)ui_chat_show_bubble(idx, y, msg->meta.dir);
    }

    hal_ui_text(74U, 54U, "BTN=menu", HAL_UI_WHITE);

    return CEEPEW_OK;
}

/* Sprint 11: Helper function to display character pool and counters.
 * Shows available character budget for message composition.
 * Displays character categories (letters, digits, punctuation) and remaining count.
 * No dynamic allocation; static buffers.
 * Two CEEPEW_ASSERTs for validation.
 */
CeePewErr_t ui_chat_show_pool(uint8_t char_budget)
{
    CEEPEW_ASSERT(char_budget <= CEEPEW_MAX_MSG_CHARS, CEEPEW_ERR_BOUNDS);

    /* Display character categories available for selection */
    char budget_str[16];
    (void)snprintf(budget_str, sizeof(budget_str), "Chars: %u/%u", char_budget, CEEPEW_MAX_MSG_CHARS);
    hal_ui_text(4U, 48U, budget_str, HAL_UI_WHITE);

    /* Draw character pool indicator bar (visual representation) */
    HalUIRect_t pool_bar = {.x = 4U, .y = 56U, .w = 120U, .h = 6U};
    hal_ui_rect(&pool_bar, HAL_UI_WHITE);

    /* Fill proportion based on remaining budget */
    CEEPEW_ASSERT(CEEPEW_MAX_MSG_CHARS > 0U, CEEPEW_ERR_BOUNDS);
    uint8_t fill_width = (uint8_t)((uint16_t)char_budget * 116U / CEEPEW_MAX_MSG_CHARS);
    if (fill_width > 116U) { fill_width = 116U; }
    HalUIRect_t pool_fill = {.x = 6U, .y = 58U, .w = fill_width, .h = 2U};
    hal_ui_rect_fill(&pool_fill, HAL_UI_WHITE);

    return CEEPEW_OK;
}

/* Sprint 11: Helper function to display message compose UI with character selector.
 * Shows current character selection (pot-based), selected character preview, and compose state.
 * No dynamic allocation; static selector and character buffers.
 * Two CEEPEW_ASSERTs for bounds checking.
 */
CeePewErr_t ui_chat_show_compose(uint8_t pot_value, uint8_t selected_idx)
{
    CEEPEW_ASSERT(selected_idx <= CEEPEW_MAX_MSG_CHARS, CEEPEW_ERR_BOUNDS);

    /* Mirror the live compose selector, including DEL and cursor controls. */
    uint8_t char_idx = (uint8_t)(((uint16_t)pot_value * COMPOSE_TOTAL_CHOICES) / 256U);
    if (char_idx >= COMPOSE_TOTAL_CHOICES) {
        char_idx = (uint8_t)(COMPOSE_TOTAL_CHOICES - 1U);
    }

    /* Display selector prompt */
    hal_ui_text(4U, 14U, "Select char:", HAL_UI_WHITE);

    char label_buf[4U];
    const char *label = compose_choice_label(char_idx, label_buf);
    uint8_t label_len = (uint8_t)strlen(label);
    uint8_t box_w = (uint8_t)((label_len * 6U) + 10U);
    if (box_w < 18U) { box_w = 18U; }
    if (box_w > 40U) { box_w = 40U; }
    uint8_t box_x = (uint8_t)((128U - box_w) / 2U);
    uint8_t text_x = (uint8_t)(box_x + ((box_w - (label_len * 6U)) / 2U));

    /* Display current selection (large, centered) */
    hal_ui_text(text_x, 20U, label, HAL_UI_WHITE);

    /* Draw selection highlight box */
    HalUIRect_t select_box = {.x = box_x, .y = 18U, .w = box_w, .h = 16U};
    hal_ui_rect(&select_box, HAL_UI_WHITE);

    /* Display instructions */
    hal_ui_text(4U, 38U, "Turn pot to select", HAL_UI_WHITE);
    hal_ui_text(8U, 46U, "Press to add char", HAL_UI_WHITE);

    return CEEPEW_OK;
}


/* Sprint 12: Helper function to display cryptogram with grouped hex rows.
 * Displays 16-byte commitment as grouped hex (2-byte groups) across the OLED.
 * Centered on display in monospace font.
 * No dynamic allocation; static hex conversion buffer.
 * Two CEEPEW_ASSERTs for bounds checking.
 */
CeePewErr_t ui_crypto_show_cryptogram(const uint8_t commitment[CEEPEW_COMMITMENT_BYTES])
{
    CEEPEW_ASSERT(commitment != NULL, CEEPEW_ERR_NULL_PTR);
    hal_ui_clear();
    hal_ui_text(16U, 2U, "COMMITMENT CODE", HAL_UI_WHITE);
    /* Grouped rows keep the full 32-byte commitment readable on 128px OLED */
    ui_draw_hex_rows(commitment, CEEPEW_COMMITMENT_BYTES, 10U, 24U);

    return CEEPEW_OK;
}

/* Sprint 12: Helper function to display cryptogram status.
 * Shows visual indicator (checkmark or X) and status text.
 * status: 0=waiting for peer, 1=match (✓), 2=mismatch (✗)
 * No dynamic allocation; static buffers.
 * Two CEEPEW_ASSERTs for validation.
 */
CeePewErr_t ui_crypto_show_status(uint8_t status)
{
    CEEPEW_ASSERT(status <= 2U, CEEPEW_ERR_BOUNDS);

    uint8_t y_pos = Y_STATUS;

    if (status == 0U) {
        /* Waiting for peer */
        ui_draw_text_wrapped(8U, y_pos, "Waiting for peer...", 120U, 8U);
    } else if (status == 1U) {
        /* Match - display checkmark and "MATCH" */
        hal_ui_char(8U, y_pos, (char)251, HAL_UI_WHITE);  /* checkmark symbol */
        hal_ui_text(20U, y_pos, "MATCH", HAL_UI_WHITE);
    } else {
        /* Mismatch - display X and "MISMATCH" */
        hal_ui_char(8U, y_pos, 'X', HAL_UI_WHITE);
        hal_ui_text(20U, y_pos, "MISMATCH", HAL_UI_WHITE);
    }

    return CEEPEW_OK;
}

/* Sprint 12: Helper function to display cryptogram confirmation UI.
 * Displays confirmation button prompt and countdown timer.
 * countdown_sec: seconds remaining (0-60)
 * No dynamic allocation; static text buffers.
 * Two CEEPEW_ASSERTs for bounds checking.
 */
CeePewErr_t ui_crypto_show_confirm(uint8_t countdown_sec)
{
    CEEPEW_ASSERT(countdown_sec <= 60U, CEEPEW_ERR_BOUNDS);

    /* Display prompt */
    hal_ui_text(4U, 8U, "Confirm match?", HAL_UI_WHITE);

    /* Draw confirmation box */
    HalUIRect_t confirm_box = {.x = 20U, .y = 20U, .w = 88U, .h = 20U};
    hal_ui_rect(&confirm_box, HAL_UI_WHITE);

    /* Display instruction */
    hal_ui_text(28U, 26U, "Press button", HAL_UI_WHITE);

    /* Display countdown timer */
    char countdown_str[16];
    (void)snprintf(countdown_str, sizeof(countdown_str), "%u sec", (unsigned int)countdown_sec);
    hal_ui_text(40U, 46U, countdown_str, HAL_UI_WHITE);

    return CEEPEW_OK;
}

/* Helper: render code incorrect (mismatch) screen */
static CeePewErr_t render_code_incorrect(void)
{
    hal_ui_clear();
    uint32_t f = g_ui_ctx.anim.frame_count;

    hal_ui_text(22U, 8U, "CODE MISMATCH", HAL_UI_WHITE);
    draw_hline(0U, 18U, 128U);

    /* Instructions */
    hal_ui_text(8U, 30U, "Codes did not match.", HAL_UI_WHITE);
    hal_ui_text(8U, 40U, "D=Retry    S=Cancel", HAL_UI_WHITE);

    /* Button handling is done in ui_manager_update() — animate prompt */
    uint8_t blink = (uint8_t)((f / 8U) % 2U);
    if (blink == 0U) { draw_hline(8U, 46U, 112U); }

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

/* Helper: render code different screen — subtle variant */
static CeePewErr_t render_code_different(void)
{
    hal_ui_clear();
    uint32_t f = g_ui_ctx.anim.frame_count;

    hal_ui_text(12U, 8U, "CODE DIFFER", HAL_UI_WHITE);
    draw_hline(0U, 18U, 128U);

    hal_ui_text(8U, 30U, "Remote code differs.", HAL_UI_WHITE);
    hal_ui_text(8U, 40U, "D=Re-enter  S=Back", HAL_UI_WHITE);

    uint8_t blink = (uint8_t)((f / 6U) % 2U);
    if (blink == 0U) { draw_hline(8U, 46U, 112U); }

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

static CeePewErr_t render_cryptogram(void)
{
    hal_ui_clear();

    uint32_t f = g_ui_ctx.anim.frame_count;

    /* Retrieve local commitment */
    CeePewErr_t err = session_get_commitment(g_ui_ctx.commitment);
    if (err != CEEPEW_OK) {
        hal_ui_text(8U, 20U, "Commit error", HAL_UI_WHITE);
        (void)hal_ui_flush();
        return err;
    }

    /* Title */
    hal_ui_text(20U, 2U, "COMMITMENT CODE", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);
    hal_ui_text(4U, 13U, "SHA-256 / 32B", HAL_UI_WHITE);

    /* Hex rows at original position y=20 to leave room for status text */
    ui_draw_hex_rows(g_ui_ctx.commitment, CEEPEW_COMMITMENT_BYTES, 10U, 20U);

    /* Determine match status */
    uint8_t status = 0U;
    if (g_ble_ctx.commitment_verified) {
        uint8_t match = 1U;
        for (uint8_t i = 0U; i < CEEPEW_COMMITMENT_BYTES; i++) {
            if (g_ui_ctx.commitment[i] != g_ble_ctx.commitment_digest[i]) {
                match = 0U; break;
            }
        }
        status = match ? 1U : 2U;
        g_ui_ctx.commitment_verified = (match == 1U);
    } else {
        /* FIX C: Early mismatch detection — before waiting, check if peer
         * commitment has arrived. If so, compare immediately. This prevents
         * infinite "Waiting for peer" if codes don't match.
         * Only compare if peer_commitment_len > 0, meaning the write succeeded. */
        uint8_t peer_has_commitment = 0U;
        for (uint8_t i = 0U; i < CEEPEW_COMMITMENT_BYTES; i++) {
            peer_has_commitment |= g_ble_ctx.commitment_digest[i];
        }
        if (peer_has_commitment != 0U) {
            uint8_t match = 1U;
            for (uint8_t i = 0U; i < CEEPEW_COMMITMENT_BYTES; i++) {
                if (g_ui_ctx.commitment[i] != g_ble_ctx.commitment_digest[i]) {
                    match = 0U; break;
                }
            }
            /* Set status to mismatch (2) if codes differ */
            status = match ? 1U : 2U;
        }
    }

    if (status == 0U) {
        /* Waiting — animated spinner at left, moved to bottom row for visibility */
        const char *spin = "|/-\\";
        char sp[2U] = { spin[(f / 4U) % 4U], '\0' };
        hal_ui_text(4U, Y_STATUS, sp, HAL_UI_WHITE);
        hal_ui_text(14U, Y_STATUS, "Waiting for peer", HAL_UI_WHITE);

        /* Check timeout: 30 seconds from pairing_start_ms */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        uint32_t elapsed = (g_ui_ctx.pairing_start_ms != 0U &&
                           now_ms >= g_ui_ctx.pairing_start_ms)
                         ? (now_ms - g_ui_ctx.pairing_start_ms) : 0U;
        if (elapsed >= 30000U) {
            /* Timeout: transition to failed state */
            g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_TIMED_OUT;
            (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
            g_ui_ctx.transition_ready = true;
            (void)hal_ui_flush();
            return CEEPEW_OK;
        }

        /* Sweeping underline (moved down with text) */
        uint8_t ul_x = (uint8_t)((f * 3U) % 128U);
        draw_hline(ul_x, 52U, 10U);

    } else if (status == 1U) {
        /* MATCH — expanding rings from centre of display */
        hal_ui_text(4U, 40U, "\x7e MATCH \x7e", HAL_UI_WHITE);

        uint8_t ring_r = (uint8_t)((f % 16U) * 2U);
        if (ring_r > 0U && ring_r < 30U) {
            draw_circle(64, 44, ring_r);
        }
        uint8_t ring2_r = (uint8_t)(((f + 8U) % 16U) * 2U);
        if (ring2_r > 0U && ring2_r < 30U) {
            draw_circle(64, 44, ring2_r);
        }

        /* Confirmation sub-panel: show sync status */
        uint32_t now_ms   = (uint32_t)(esp_timer_get_time() / 1000LL);
        uint32_t elapsed  = (g_ui_ctx.crypto_confirm_start_ms != 0U &&
                             now_ms >= g_ui_ctx.crypto_confirm_start_ms)
                          ? (now_ms - g_ui_ctx.crypto_confirm_start_ms) : 0U;
        uint32_t rem_ms   = (elapsed < 30000U) ? (30000U - elapsed) : 0U;

        /* Check peer readiness: both must signal ready before transition */
        if (transport_ble_both_ready_for_chat()) {
            char cd_str[20U];
            (void)snprintf(cd_str, sizeof(cd_str), "Ready! Press btn");
            hal_ui_text(12U, 54U, cd_str, HAL_UI_WHITE);

            if (g_ui_ctx.button_pressed) {
                (void)ui_manager_transition_to(UI_STATE_CHAT);
                g_ui_ctx.transition_ready = true;
            }
        } else {
            /* Waiting for peer to signal ready — show simple text and visual cue */
            hal_ui_text(20U, 54U, "Syncing...", HAL_UI_WHITE);

            if (rem_ms == 0U) {
                /* Timeout: transition anyway */
                (void)ui_manager_transition_to(UI_STATE_CHAT);
                g_ui_ctx.transition_ready = true;
            }
        }

    } else {
        /* MISMATCH — pulsing X with bracket animation */
        uint8_t blink = (uint8_t)((f / 5U) % 2U);
        if (blink == 0U) {
            hal_ui_text(4U,  48U, ">", HAL_UI_WHITE);
            hal_ui_text(14U, 48U, "MISMATCH", HAL_UI_WHITE);
            hal_ui_text(62U, 48U, "<", HAL_UI_WHITE);
        }
        hal_ui_text(4U, 56U, "Restart to re-pair", HAL_UI_WHITE);
    }

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

/* Phase 4: Chat menu — user selects Read/Write/Check with potentiometer-driven pointer */
static CeePewErr_t render_chat_menu(void)
{
    hal_ui_clear();

    /* Title */
    hal_ui_text(20U, 1U, "SECURE CHAT", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    /* Two options only */
    static const char *const OPTS[2U] = {
        " WRITE MESSAGE ",
        "  VIEW THREAD  "
    };

    /* Map the full potentiometer travel across the visible options. */
    uint8_t sel = ui_map_pot_to_index(g_ui_ctx.user_input, 2U);
    g_ui_ctx.chat_menu_selected = sel;

    for (uint8_t i = 0U; i < 2U; i++) {
        uint8_t y = (uint8_t)(22U + i * 20U);
        draw_selected_option_row(8U, y, 112U, 14U, OPTS[i], (i == sel));
    }

    /* Message count badge */
    uint8_t cnt = (uint8_t)msg_store_count();
    if (cnt > 0U) {
        char badge[12U];
        (void)snprintf(badge, sizeof(badge), "[%u msg]", (unsigned)cnt);
        hal_ui_text(4U, 54U, badge, HAL_UI_WHITE);
    }

    hal_ui_text(66U, 54U, "BTN=select", HAL_UI_WHITE);

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

/* Phase 4: Chat compose — cursor-aware editor with selectable commands */
static CeePewErr_t render_chat_compose(void)
{
    CEEPEW_ASSERT(s_ui_manager_initialised, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(g_ui_ctx.compose_length <= 255U, CEEPEW_ERR_BOUNDS);

    hal_ui_clear();
    uint8_t choice_idx = (uint8_t)(((uint16_t)g_ui_ctx.user_input * COMPOSE_TOTAL_CHOICES) / 256U);
    if (choice_idx >= COMPOSE_TOTAL_CHOICES) {
        choice_idx = (uint8_t)(COMPOSE_TOTAL_CHOICES - 1U);
    }
    g_ui_ctx.keyboard_col = choice_idx;
    compose_terminate_buffer();

    /* ── Status bar ───────────────────────────────────────────────── */
    char hdr[24U];
    (void)snprintf(hdr, sizeof(hdr), "WRITE [%u/255]", (unsigned)g_ui_ctx.compose_length);
    hal_ui_text(0U, 0U, hdr, HAL_UI_WHITE);
    draw_hline(0U, 9U, 128U);

    /* ── Selected action/character ───────────────────────────────── */
    char label_buf[4U];
    const char *sel_label = compose_choice_label(choice_idx, label_buf);
    uint8_t label_len = (uint8_t)strlen(sel_label);
    uint8_t box_w = (uint8_t)((label_len * 6U) + 10U);
    if (box_w < 18U) { box_w = 18U; }
    if (box_w > 42U) { box_w = 42U; }
    uint8_t box_x = (uint8_t)((128U - box_w) / 2U);
    uint8_t text_x = (uint8_t)(box_x + ((box_w - (label_len * 6U)) / 2U));
    HalUIRect_t sel_box = { .x = box_x, .y = 13U, .w = box_w, .h = 16U };
    hal_ui_rect(&sel_box, HAL_UI_WHITE);
    if (choice_idx < COMPOSE_CHAR_COUNT) {
        char cs[2U] = { sel_label[0], '\0' };
        hal_ui_text(text_x, 17U, cs, HAL_UI_WHITE);
        hal_ui_text((uint8_t)(text_x - 1U), 16U, cs, HAL_UI_WHITE);
    } else {
        hal_ui_text(text_x, 18U, sel_label, HAL_UI_WHITE);
    }

    /* Neighbour hints */
    if (choice_idx > 0U) {
        char prev_buf[4U];
        const char *prev = compose_choice_label((uint8_t)(choice_idx - 1U), prev_buf);
        hal_ui_text(38U, 18U, prev, HAL_UI_WHITE);
    }
    if (choice_idx + 1U < COMPOSE_TOTAL_CHOICES) {
        char next_buf[4U];
        const char *next = compose_choice_label((uint8_t)(choice_idx + 1U), next_buf);
        hal_ui_text(76U, 18U, next, HAL_UI_WHITE);
    }

    const char *cat = (choice_idx < 26U)   ? "A-Z"
                    : (choice_idx < 36U)   ? "0-9"
                    : (choice_idx < COMPOSE_CHAR_COUNT) ? "PUNCT"
                    : (choice_idx == COMPOSE_ACTION_DEL_IDX) ? "DEL"
                    : (choice_idx == COMPOSE_ACTION_LEFT_IDX) ? "LEFT"
                    : (choice_idx == COMPOSE_ACTION_RIGHT_IDX) ? "RIGHT"
                    : "EDIT";
    char cat_str[16U];
    (void)snprintf(cat_str, sizeof(cat_str), "[%s]", cat);
    hal_ui_text(46U, 30U, cat_str, HAL_UI_WHITE);

    /* ── Message preview with cursor ─────────────────────────────── */
    draw_hline(0U, 36U, 128U);
    render_compose_preview_line();

    /* ── Footer controls ─────────────────────────────────────────── */
    draw_hline(0U, 47U, 128U);
    hal_ui_text(0U, 48U, "TAP=use sel", HAL_UI_WHITE);
    hal_ui_text(0U, 56U, "HOLD=send confirm", HAL_UI_WHITE);
    hal_ui_text(92U, 56U, "DEL/<-/->", HAL_UI_WHITE);

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

static CeePewErr_t render_chat_send_confirm(void)
{
    hal_ui_clear();
    g_ui_ctx.chat_send_confirm_selected = ui_map_pot_to_index(g_ui_ctx.user_input, 2U);
    compose_terminate_buffer();

    hal_ui_text(20U, 1U, "SEND MESSAGE", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);
    hal_ui_text(4U, 14U, "send message:", HAL_UI_WHITE);

    char preview[24U];
    (void)hal_ui_fit_text(g_ui_ctx.compose_buffer, 120U, preview, sizeof(preview));
    hal_ui_text(4U, 22U, preview, HAL_UI_WHITE);

    draw_selected_option_row(4U, 38U, 54U, 16U, "SEND", (g_ui_ctx.chat_send_confirm_selected == 0U));
    hal_ui_text(8U, 42U, "\xFB", HAL_UI_BLACK);
    draw_selected_option_row(70U, 38U, 54U, 16U, "GO BACK", (g_ui_ctx.chat_send_confirm_selected != 0U));
    hal_ui_text(74U, 42U, "X", HAL_UI_BLACK);

    hal_ui_text(14U, 56U, "tap choice", HAL_UI_WHITE);

    return CEEPEW_OK;
}

CeePewErr_t ui_manager_update(void)
{
    CEEPEW_ASSERT(s_ui_manager_initialised, CEEPEW_ERR_PARAM);

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    bool state_changed = false;

    /* Check for state transitions */
    if (g_ui_ctx.transition_ready && g_ui_ctx.next_state != g_ui_ctx.current_state) {
        g_ui_ctx.current_state = g_ui_ctx.next_state;
        g_ui_ctx.anim.frame_count = 0U;
        g_ui_ctx.transition_ready = false;
        state_changed = true;

        /* State-entry initialisation */
        if (g_ui_ctx.current_state == UI_STATE_CODE_ENTRY) {
            /* Reset code entry context */
            for (uint8_t i = 0U; i < 4U; i++) { g_ui_ctx.code_digits[i] = (uint8_t)'0'; }
            g_ui_ctx.code_selected = 0U;
            g_ui_ctx.button_prev = false;
            g_ui_ctx.button_press_start_ms = 0U;
            g_ui_ctx.code_entry_start_ms = now_ms;
            g_ui_ctx.countdown_start_ms = 0U;
            g_ui_ctx.pairing_start_ms = 0U;
            g_ui_ctx.pairing_result_start_ms = 0U;
            g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_NONE;
        } else if (g_ui_ctx.current_state == UI_STATE_PAIRING) {
            g_ui_ctx.pairing_start_ms = now_ms;
            g_ui_ctx.countdown_start_ms = now_ms;
            g_ui_ctx.pairing_result_start_ms = 0U;
            g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_NONE;
        } else if (g_ui_ctx.current_state == UI_STATE_PAIRING_SUCCESS) {
            g_ui_ctx.pairing_result_start_ms = now_ms;
            g_ui_ctx.button_prev = false;
        } else if (g_ui_ctx.current_state == UI_STATE_PAIRING_FAILED) {
            g_ui_ctx.pairing_result_start_ms = now_ms;
            g_ui_ctx.button_prev = false;
            g_ui_ctx.reject_sequence_start_ms = 0U;
            g_ui_ctx.error_start_ms = 0U;
            (void)transport_ble_disconnect();
            /* Trigger red LED pulse for pairing failure */
            (void)rgb_set_pattern(RGB_RED_BLINK);

            /* Bug 5 Fix: Force immediate restart of advertising/scan so device is
             * re-discoverable for reparing attempts. This ensures the peer doesn't
             * expire during the hold period and can rediscover us quickly. */
            (void)transport_ble_restart_discovery_session();
        } else if (g_ui_ctx.current_state == UI_STATE_DISCOVERY) {
            /* BLE is initialized once by app_main; discovery entry only
             * restarts advertising if the transport is idle. */
            BleState_t ble_state = transport_ble_get_state();
            if (ble_state == BLE_IDLE) {
                CeePewErr_t ble_err = transport_ble_start_advertising();
                if (ble_err != CEEPEW_OK) {
                    ESP_LOGE("ui", "start_advertising failed: %d", (int)ble_err);
                    g_ui_ctx.current_state = UI_STATE_ERROR;
                    g_ui_ctx.error_start_ms = now_ms;
                    return CEEPEW_OK;
                }
                /* FIX: also restart scanning so both directions work after re-entry */
                (void)transport_ble_start_scan();
            } else if (ble_state == BLE_ADVERTISING) {
                /* Advertising is up but scan may have been lost — restart it */
                (void)transport_ble_start_scan();
            }
            g_ui_ctx.countdown_start_ms = 0U;
            g_ui_ctx.pairing_start_ms = 0U;
        } else if (g_ui_ctx.current_state == UI_STATE_COUNTDOWN ||
                   g_ui_ctx.current_state == UI_STATE_PAIRING) {
            /* Mark countdown start time */
            g_ui_ctx.countdown_start_ms = now_ms;
            g_ui_ctx.pairing_start_ms = now_ms;
        } else if (g_ui_ctx.current_state == UI_STATE_KEYDER) {
            g_ui_ctx.fingerprint_confirmed = false;
        } else if (g_ui_ctx.current_state == UI_STATE_FINGERPRINT) {
            g_ui_ctx.fingerprint_confirmed = false;
            if (session_get_fingerprint(g_ui_ctx.fingerprint) != CEEPEW_OK) {
                memset(g_ui_ctx.fingerprint, 0U, sizeof(g_ui_ctx.fingerprint));
            }
        } else if (g_ui_ctx.current_state == UI_STATE_FINGERPRINT_CONFIRM) {
            g_ui_ctx.fingerprint_confirmed = false;
            g_ui_ctx.button_prev = false;
            g_ui_ctx.reject_sequence_start_ms = 0U;
            g_ui_ctx.error_start_ms = 0U;
        } else if (g_ui_ctx.current_state == UI_STATE_CRYPTOGRAM) {
            /* Initialize cryptogram confirmation context */
            memset(g_ui_ctx.commitment, 0U, CEEPEW_COMMITMENT_BYTES);
            memset(g_ui_ctx.peer_commitment, 0U, CEEPEW_COMMITMENT_BYTES);
            /* FIX: seed peer commitment from BLE context so renderer can compare */
            memcpy(g_ui_ctx.peer_commitment, g_ble_ctx.commitment_digest, CEEPEW_COMMITMENT_BYTES);
            g_ui_ctx.commitment_verified   = g_ble_ctx.commitment_verified;
            g_ui_ctx.crypto_confirm_start_ms = now_ms;

            /* Signal readiness after commitment verified, enabling sync handshake with peer */
            if (g_ble_ctx.commitment_verified && !g_ble_ctx.ready_for_chat) {
                transport_ble_set_ready_for_chat();
                ESP_LOGI("ui", "Cryptogram state: signaled ready_for_chat to peer");
            }
        } else if (g_ui_ctx.current_state == UI_STATE_CHAT) {
            g_ui_ctx.button_prev = false;
        } else if (g_ui_ctx.current_state == UI_STATE_CHAT_MENU) {
            /* Initialize chat menu */
            g_ui_ctx.chat_menu_selected = 0U;
            g_ui_ctx.button_prev = false;
        } else if (g_ui_ctx.current_state == UI_STATE_CHAT_COMPOSE) {
            compose_reset_cursor_if_needed();
            compose_terminate_buffer();
            g_ui_ctx.button_prev = false;
            g_ui_ctx.button_press_start_ms = 0U;
        } else if (g_ui_ctx.current_state == UI_STATE_CHAT_SEND_CONFIRM) {
            g_ui_ctx.chat_send_confirm_selected = 0U;
            g_ui_ctx.button_prev = false;
        } else if (g_ui_ctx.current_state == UI_STATE_NONCE_EXHAUSTED) {
            g_ui_ctx.error_start_ms = now_ms;
        } else if (g_ui_ctx.current_state == UI_STATE_ERROR) {
            if (g_ui_ctx.reject_sequence_start_ms != 0U || g_ui_ctx.error_start_ms == 0U) {
                g_ui_ctx.reject_sequence_start_ms = now_ms;
                g_ui_ctx.error_start_ms = now_ms;
            }
        }

        CeePewErr_t layout_err = layout_validate_state_entry(g_ui_ctx.current_state);
        if (layout_err != CEEPEW_OK) {
            ESP_LOGE("ui", "layout validation failed for state=%s err=%d",
                     layout_state_name(g_ui_ctx.current_state), (int)layout_err);
        }
    }

    if (g_ui_ctx.current_state == UI_STATE_PAIRING_SUCCESS) {
        if (g_ui_ctx.pairing_result_start_ms != 0U &&
            (now_ms - g_ui_ctx.pairing_result_start_ms) >= CEEPEW_PAIRING_SUCCESS_HOLD_MS) {
            g_ui_ctx.pairing_result_start_ms = 0U;
            (void)ui_manager_transition_to(UI_STATE_KEYDER);
            g_ui_ctx.transition_ready = true;
        }
    } else if (g_ui_ctx.current_state == UI_STATE_PAIRING_FAILED) {
        /* Spec §4: PAIRING_FAILED persists until button press.
         * The user must explicitly acknowledge the failure before the
         * device returns to discovery. The legacy auto-restart timer
         * (CEEPEW_PAIRING_FAILED_HOLD_MS) is removed to match the spec. */
        if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
            g_ui_ctx.button_press_start_ms = now_ms;
        } else if (!g_ui_ctx.button_pressed && g_ui_ctx.button_prev) {
            g_ui_ctx.pairing_result_start_ms = 0U;
            (void)ui_restart_discovery_from_pairing();
            g_ui_ctx.transition_ready = true;
        }
    }

    if (g_ui_ctx.diag_mode) {
        uint8_t diag_page = ui_map_pot_to_index(g_ui_ctx.user_input, s_diag_page_count);

        if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
            g_ui_ctx.button_press_start_ms = now_ms;
        } else if (!g_ui_ctx.button_pressed && g_ui_ctx.button_prev) {
            uint32_t dur = (now_ms >= g_ui_ctx.button_press_start_ms)
                         ? (now_ms - g_ui_ctx.button_press_start_ms) : 0U;

            if (diag_page == 3U) {
                if (dur >= 1800U) {
                    session_wipe();
                } else if (dur >= 700U) {
                    (void)ui_manager_reset_to_discovery();
                } else {
                    g_ui_ctx.anim.frame_count = 0U;
                }
            } else if (dur < 500U) {
                g_ui_ctx.anim.frame_count = 0U;
            }
        }

        g_ui_ctx.button_prev = g_ui_ctx.button_pressed;
        return CEEPEW_OK;
    }

    /* Input handling for interactive states */
    if (state_changed) {
        g_ui_ctx.button_prev = g_ui_ctx.button_pressed;
    } else if (g_ui_ctx.current_state == UI_STATE_CODE_ENTRY) {
        /* Map pot value (0-255) to extended charset 0-9,A-Z (36 symbols) */
        uint8_t pot = g_ui_ctx.user_input;
        static const char CODE_CHARSET[37] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"; /* 36 + NUL */
        uint8_t idx = (uint8_t)(((uint16_t)pot * 36U) / 256U); /* 0..35 */
        if (idx >= 36U) { idx = 35U; }
        if (g_ui_ctx.code_selected < 4U) {
            g_ui_ctx.code_digits[g_ui_ctx.code_selected] = (uint8_t)CODE_CHARSET[idx];
        }

        /* Button edge detection for short-press vs hold */
        if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
            /* Button just pressed */
            g_ui_ctx.button_press_start_ms = now_ms;
        } else if (!g_ui_ctx.button_pressed && g_ui_ctx.button_prev) {
            /* Button just released: check duration */
            uint32_t dur = 0U;
            if (now_ms >= g_ui_ctx.button_press_start_ms) { dur = now_ms - g_ui_ctx.button_press_start_ms; }

            const uint32_t HOLD_MS = 1000U; /* press-and-hold threshold */
            const uint32_t ENTRY_ARM_MS = 700U;
            if (dur >= HOLD_MS &&
                g_ui_ctx.code_entry_start_ms != 0U &&
                now_ms >= g_ui_ctx.code_entry_start_ms &&
                (now_ms - g_ui_ctx.code_entry_start_ms) >= ENTRY_ARM_MS) {
                if (transport_ble_has_peer_cached()) {
                    /* Confirm code and start countdown */
                    (void)ui_manager_transition_to(UI_STATE_PAIRING);
                    g_ui_ctx.pairing_start_ms = now_ms;
                    g_ui_ctx.countdown_start_ms = now_ms;
                    g_ui_ctx.transition_ready = true;
                } else {
                    ESP_LOGW("ui", "pairing confirm ignored: peer unavailable");
                    g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_LINK_FAIL;
                    (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
                    g_ui_ctx.transition_ready = true;
                }
            } else {
                /* Short press: move to next digit */
                if (g_ui_ctx.code_selected < 3U) { g_ui_ctx.code_selected++; }
                else { /* wrap to first digit */ g_ui_ctx.code_selected = 0U; }
            }
        }
    } else if (g_ui_ctx.current_state == UI_STATE_DISCOVERY) {
       /* Button press during discovery: if peer found, connect and transition */
       if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
           if (transport_ble_get_peer() != NULL) {
               (void)ui_manager_transition_to(UI_STATE_CODE_ENTRY);
               g_ui_ctx.transition_ready = true;
           }
           /* If peer not yet discovered, ignore button press (no-op) */
       }
    } else if (g_ui_ctx.current_state == UI_STATE_CODE_INCORRECT) {
        /* Code mismatch screen: short press = retry, long hold = cancel */
        if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
            g_ui_ctx.button_press_start_ms = now_ms;
        } else if (!g_ui_ctx.button_pressed && g_ui_ctx.button_prev) {
            uint32_t dur = (now_ms >= g_ui_ctx.button_press_start_ms) ? (now_ms - g_ui_ctx.button_press_start_ms) : 0U;
            if (dur >= 1000U) {
                /* Cancel and return to discovery */
                (void)ui_manager_transition_to(UI_STATE_DISCOVERY);
                g_ui_ctx.transition_ready = true;
            } else {
                /* Retry: go back to code entry */
                (void)ui_manager_transition_to(UI_STATE_CODE_ENTRY);
                g_ui_ctx.transition_ready = true;
            }
        }
    } else if (g_ui_ctx.current_state == UI_STATE_CODE_DIFFERENT) {
        /* Remote code different: short press = re-enter, long hold = back to discovery */
        if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
            g_ui_ctx.button_press_start_ms = now_ms;
        } else if (!g_ui_ctx.button_pressed && g_ui_ctx.button_prev) {
            uint32_t dur = (now_ms >= g_ui_ctx.button_press_start_ms) ? (now_ms - g_ui_ctx.button_press_start_ms) : 0U;
            if (dur >= 1000U) {
                (void)ui_manager_transition_to(UI_STATE_DISCOVERY);
                g_ui_ctx.transition_ready = true;
            } else {
                (void)ui_manager_transition_to(UI_STATE_CODE_ENTRY);
                g_ui_ctx.transition_ready = true;
            }
        }
    } else if (g_ui_ctx.current_state == UI_STATE_PAIRING) {
        /* Pairing countdown: 3 s hold on the button = DEBUG hook that
         * forces the supervisor to run its recovery path. The RGB LED
         * should immediately switch to RGB_YELLOW_RED_BLINK and the
         * device should re-advertise / re-scan. Useful for verifying
         * Task 2 (radio-failure recovery) without a broken device. */
        if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
            g_ui_ctx.button_press_start_ms = now_ms;
        } else if (!g_ui_ctx.button_pressed && g_ui_ctx.button_prev) {
            uint32_t dur = (now_ms >= g_ui_ctx.button_press_start_ms)
                            ? (now_ms - g_ui_ctx.button_press_start_ms) : 0U;
            if (dur >= 3000U) {
                ESP_LOGW("ui", "Pairing screen: 3 s hold detected — triggering debug timeout");
                (void)transport_ble_debug_trigger_timeout();
                /* Force the visual feedback bridge to re-evaluate immediately
                 * so the LED reflects the recovery state on the very next
                 * tick rather than waiting for a phase transition. */
                task_ui_update_visual_feedback();
            }
            /* No short-press action — pairing is non-interactive once
             * the user has confirmed the code. Short press is ignored
             * to avoid accidental state churn. */
        }
    } else if (g_ui_ctx.current_state == UI_STATE_FINGERPRINT) {
        if (g_ui_ctx.diag_mode) {
            (void)ui_manager_transition_to(UI_STATE_ERROR);
            g_ui_ctx.reject_sequence_start_ms = now_ms;
            g_ui_ctx.error_start_ms = now_ms;
            g_ui_ctx.transition_ready = true;
        } else if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
            (void)ui_manager_transition_to(UI_STATE_FINGERPRINT_CONFIRM);
            g_ui_ctx.transition_ready = true;
        }
    } else if (g_ui_ctx.current_state == UI_STATE_FINGERPRINT_CONFIRM) {
        if (g_ui_ctx.diag_mode) {
            (void)ui_manager_transition_to(UI_STATE_ERROR);
            g_ui_ctx.reject_sequence_start_ms = now_ms;
            g_ui_ctx.error_start_ms = now_ms;
            g_ui_ctx.transition_ready = true;
        } else if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
            g_ui_ctx.fingerprint_confirmed = true;
            (void)ui_manager_transition_to(UI_STATE_CHAT);
            g_ui_ctx.transition_ready = true;
        }
    } else if (g_ui_ctx.current_state == UI_STATE_CHAT) {
        /* Chat thread: short press opens menu, long hold returns to cryptogram */
        if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
            g_ui_ctx.button_press_start_ms = now_ms;
        } else if (!g_ui_ctx.button_pressed && g_ui_ctx.button_prev) {
            uint32_t dur = (now_ms >= g_ui_ctx.button_press_start_ms)
                           ? (now_ms - g_ui_ctx.button_press_start_ms) : 0U;
            if (dur >= 1500U) {
                (void)ui_manager_transition_to(UI_STATE_CRYPTOGRAM);
                g_ui_ctx.transition_ready = true;
            } else {
                (void)ui_manager_transition_to(UI_STATE_CHAT_MENU);
                g_ui_ctx.transition_ready = true;
            }
        }
    } else if (g_ui_ctx.current_state == UI_STATE_CHAT_MENU) {
        /* Chat menu: button selects option */
        if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
            g_ui_ctx.button_press_start_ms = now_ms;
        } else if (!g_ui_ctx.button_pressed && g_ui_ctx.button_prev) {
            uint32_t dur = (now_ms >= g_ui_ctx.button_press_start_ms)
                           ? (now_ms - g_ui_ctx.button_press_start_ms) : 0U;
            if (dur >= 1500U) {
                /* Long hold in menu → back to cryptogram to re-check state */
                (void)ui_manager_transition_to(UI_STATE_CRYPTOGRAM);
                g_ui_ctx.transition_ready = true;
            } else {
                /* Short press → act on selected menu option */
                if (g_ui_ctx.chat_menu_selected == 0U) {
                    /* Write — enter compose */
                    (void)ui_manager_transition_to(UI_STATE_CHAT_COMPOSE);
                    g_ui_ctx.transition_ready = true;
                } else {
                    /* Read — return to thread */
                    (void)ui_manager_transition_to(UI_STATE_CHAT);
                    g_ui_ctx.transition_ready = true;
                }
            }
        }
    } else if (g_ui_ctx.current_state == UI_STATE_CHAT_COMPOSE) {
        /* Message compose: tap applies selection, long press opens confirm */
        g_ui_ctx.keyboard_col = ui_map_pot_to_index(g_ui_ctx.user_input, COMPOSE_TOTAL_CHOICES);
        if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
            g_ui_ctx.button_press_start_ms = now_ms;
        } else if (!g_ui_ctx.button_pressed && g_ui_ctx.button_prev) {
            uint32_t dur = (now_ms >= g_ui_ctx.button_press_start_ms) ? (now_ms - g_ui_ctx.button_press_start_ms) : 0U;
            if (dur >= 1800U) {
                /* Long press: show confirmation screen */
                if (g_ui_ctx.compose_length > 0U) {
                    g_ui_ctx.chat_send_confirm_selected = 0U;
                    (void)ui_manager_transition_to(UI_STATE_CHAT_SEND_CONFIRM);
                    g_ui_ctx.transition_ready = true;
                }
            } else {
                /* Short press: apply selected character/command */
                CeePewErr_t edit_err = compose_commit_selection(g_ui_ctx.keyboard_col);
                if (edit_err != CEEPEW_OK) {
                    ESP_LOGW("ui", "compose selection failed: %d", (int)edit_err);
                }
            }
        }
    } else if (g_ui_ctx.current_state == UI_STATE_CHAT_SEND_CONFIRM) {
        g_ui_ctx.chat_send_confirm_selected = ui_map_pot_to_index(g_ui_ctx.user_input, 2U);
        if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
            g_ui_ctx.button_press_start_ms = now_ms;
        } else if (!g_ui_ctx.button_pressed && g_ui_ctx.button_prev) {
            if (g_ui_ctx.chat_send_confirm_selected == 0U) {
                uint8_t peer_pk[32U];
                CeePewErr_t peer_err = session_get_peer_public_key(peer_pk);
                if (peer_err == CEEPEW_OK) {
                    CeePewErr_t send_err = session_send_message(
                        (const uint8_t *)g_ui_ctx.compose_buffer,
                        (uint16_t)g_ui_ctx.compose_length,
                        g_ble_ctx.peer_mac,
                        peer_pk);
                    ceepew_secure_zero(peer_pk, sizeof(peer_pk));
                    if (send_err == CEEPEW_OK) {
                        ESP_LOGI("ui", "SEND OK: '%.*s'", (int)g_ui_ctx.compose_length, g_ui_ctx.compose_buffer);
                        for (uint8_t ci = 0U; ci < g_ui_ctx.compose_length; ci++) {
                            g_ui_ctx.compose_buffer[ci] = 0U;
                        }
                        g_ui_ctx.compose_length = 0U;
                        g_ui_ctx.compose_cursor = 0U;
                        compose_terminate_buffer();
                        (void)ui_manager_transition_to(UI_STATE_CHAT_MENU);
                        g_ui_ctx.transition_ready = true;
                    } else {
                        ESP_LOGW("ui", "session_send_message failed: %d", (int)send_err);
                        g_ui_ctx.reject_sequence_start_ms = 0U;
                        g_ui_ctx.error_start_ms = now_ms;
                        (void)ui_manager_transition_to(UI_STATE_ERROR);
                        g_ui_ctx.transition_ready = true;
                    }
                } else {
                    ESP_LOGW("ui", "session_get_peer_public_key failed: %d", (int)peer_err);
                    g_ui_ctx.reject_sequence_start_ms = 0U;
                    g_ui_ctx.error_start_ms = now_ms;
                    (void)ui_manager_transition_to(UI_STATE_ERROR);
                    g_ui_ctx.transition_ready = true;
                }
            } else {
                (void)ui_manager_transition_to(UI_STATE_CHAT_COMPOSE);
                g_ui_ctx.transition_ready = true;
            }
        }
    } else if (g_ui_ctx.current_state == UI_STATE_ERROR ||
               g_ui_ctx.current_state == UI_STATE_NONCE_EXHAUSTED) {
        if (g_ui_ctx.current_state == UI_STATE_ERROR && g_ui_ctx.reject_sequence_start_ms == 0U) {
            if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
                g_ui_ctx.button_press_start_ms = now_ms;
            } else if (!g_ui_ctx.button_pressed && g_ui_ctx.button_prev) {
                uint32_t dur = (now_ms >= g_ui_ctx.button_press_start_ms)
                             ? (now_ms - g_ui_ctx.button_press_start_ms) : 0U;
                if (dur >= 1000U) {
                    (void)ui_manager_transition_to(UI_STATE_CHAT_MENU);
                    g_ui_ctx.transition_ready = true;
                } else {
                    (void)ui_manager_transition_to(UI_STATE_CHAT_COMPOSE);
                    g_ui_ctx.transition_ready = true;
                }
            }
        } else {
            uint32_t start_ms = (g_ui_ctx.current_state == UI_STATE_ERROR)
                                ? g_ui_ctx.reject_sequence_start_ms
                                : g_ui_ctx.error_start_ms;
            if (start_ms != 0U) {
                uint32_t elapsed_ms = (now_ms >= start_ms) ? (now_ms - start_ms) : 0U;
                uint32_t seq_ms = (uint32_t)(CEEPEW_RGB_REJECT_SEQUENCE_CT * 2U * CEEPEW_RGB_ERROR_BLINK_MS);
                if (elapsed_ms >= seq_ms) {
                    /* Debounce UI-originated wipes to prevent rapid repeated wipes during transient UI animations */
                    if (now_ms - s_last_ui_wipe_ms > 5000U) {
                        ESP_LOGW("ui", "session_wipe requested by UI (error timeout) — debounced");
                        s_last_ui_wipe_ms = now_ms;
                        session_wipe();
                    }
                }
            }
        }
    }

    uint32_t now_s = now_ms / 1000U;
    if (!g_ble_ctx.gattc_connected &&
        !g_ble_ctx.gatts_connected &&
        !g_ble_ctx.connecting &&
        !session_is_active() &&
        transport_ble_get_peer() == NULL &&
        (g_ble_ctx.state == BLE_SCANNING ||
         g_ble_ctx.state == BLE_ADVERTISING ||
         g_ble_ctx.state == BLE_ADVERTISING_AND_SCANNING) &&
        g_ble_ctx.discovery_start_ts != 0U &&
        now_s >= g_ble_ctx.discovery_start_ts &&
        (now_s - g_ble_ctx.discovery_start_ts) >= CEEPEW_PAIRING_TIMEOUT_S) {
        /* Discovery timeout: do NOT perform a destructive secure wipe here.
         * Instead, gracefully abort pairing and return to discovery so the
         * device can re-advertise/scan and attempt recovery. This prevents
         * repeated secure wipes caused by transient scan/adv gaps. */
        if (now_ms - s_last_ui_wipe_ms > 5000U) {
            ESP_LOGW("ui", "discovery timeout — aborting pairing and returning to discovery (no wipe)");
            s_last_ui_wipe_ms = now_ms;
            /* Ensure BLE link is torn down and UI returns to discovery state */
            (void)transport_ble_disconnect();
            (void)ui_manager_transition_to(UI_STATE_DISCOVERY);
            g_ui_ctx.transition_ready = true;
        }
    }

    if (!g_ble_ctx.gattc_connected &&
        !g_ble_ctx.gatts_connected &&
        !session_is_active() &&
        g_ble_ctx.state == BLE_PAIRING &&
        g_ble_ctx.pairing_start_ts != 0U &&
        now_s >= g_ble_ctx.pairing_start_ts &&
        (now_s - g_ble_ctx.pairing_start_ts) >= CEEPEW_PAIRING_TIMEOUT_S) {
        if (now_ms - s_last_ui_wipe_ms > 5000U) {
            ESP_LOGW("ui", "pairing timeout — aborting pairing and returning to discovery (no wipe)");
            s_last_ui_wipe_ms = now_ms;
            (void)ui_restart_discovery_from_pairing();
        }
    }

    /* Remember previous button state for edge detection */
    g_ui_ctx.button_prev = g_ui_ctx.button_pressed;

    /* ── BLE visual feedback bridge ──────────────────────────────────────
     * While the user is on the Pairing screen, surface the BLE pairing
     * phase (or the supervisor recovery indicator) on the RGB LED so
     * the user can tell at a glance whether the link is stuck, recovering,
     * or making progress. task_ui_update_visual_feedback() itself
     * throttles writes — only called when state == PAIRING, not on
     * every tick of every screen. */
    if (g_ui_ctx.current_state == UI_STATE_PAIRING) {
        task_ui_update_visual_feedback();
    }

    return CEEPEW_OK;
}

typedef struct {
    uint8_t  cpu_load_pct;
    uint32_t heap_total_bytes;
    uint32_t heap_used_bytes;
    uint32_t heap_free_bytes;
    uint32_t loop_rate_hz;
    uint32_t psram_total_bytes;
    uint32_t psram_used_bytes;
    uint32_t uptime_seconds;
    uint32_t draw_cost_us;
} DiagMetrics_t;

static const char *diag_ble_state_name(BleState_t state)
{
    switch (state) {
        case BLE_IDLE: return "IDLE";
        case BLE_ADVERTISING: return "ADV";
        case BLE_SCANNING: return "SCAN";
        case BLE_ADVERTISING_AND_SCANNING: return "ADV+SCAN";
        case BLE_CONNECTED: return "CONN";
        case BLE_PAIRING: return "PAIR";
        case BLE_DONE: return "DONE";
        default: return "?";
    }
}

static void diag_collect_metrics(DiagMetrics_t *m)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0U, sizeof(*m));

    m->heap_total_bytes = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    m->heap_free_bytes  = esp_get_free_heap_size();
    if (m->heap_total_bytes > m->heap_free_bytes) {
        m->heap_used_bytes = m->heap_total_bytes - m->heap_free_bytes;
    }

    m->psram_total_bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (m->psram_total_bytes > 0U) {
        uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        m->psram_used_bytes = (m->psram_total_bytes > psram_free)
                            ? (m->psram_total_bytes - psram_free)
                            : 0U;
    }

    m->uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000LL);
    m->loop_rate_hz   = s_diag_last_loop_rate_hz;
    m->draw_cost_us   = s_diag_last_draw_cost_us;

    uint32_t period_us = CEEPEW_UI_LOOP_DELAY_MS * 1000U;
    if (period_us > 0U) {
        uint32_t busy = (uint32_t)(((uint64_t)m->draw_cost_us * 100ULL) / period_us);
        if (busy > 100U) {
            busy = 100U;
        }
        m->cpu_load_pct = (uint8_t)busy;
    }
}

static void diag_draw_metric_bar(uint8_t x, uint8_t y, uint8_t bar_width,
                                 const char *label, uint8_t pct,
                                 const char *value_text)
{
    uint8_t clamped = (pct > 100U) ? 100U : pct;

    /* Draw label first (e.g., "CPU", "HEAP", "PSRAM") */
    hal_ui_text(x, y, label, HAL_UI_WHITE);

    /* Calculate dynamic bar position based on label length
     * Each character is 6 pixels wide, plus 2 pixels space after label */
    uint8_t label_len = 0U;
    while (label != NULL && label[label_len] != '\0' && label_len < 10U) {
        label_len++;
    }
    uint8_t bar_x = (uint8_t)(x + (label_len * 6U) + 2U);

    /* Draw inline bar [||||    ] */
    hal_ui_text(bar_x, y, "[", HAL_UI_WHITE);

    /* Draw filled portion with pipe characters */
    uint8_t pipe_x = (uint8_t)(bar_x + 6U);
    uint8_t pipes = (uint8_t)((bar_width * clamped) / 100U / 5U);  /* ~5px per pipe char */
    if (pipes > 8U) { pipes = 8U; }  /* Max 8 pipes to fit in display */

    for (uint8_t i = 0U; i < pipes; i++) {
        hal_ui_text((uint8_t)(pipe_x + (i * 6U)), y, "|", HAL_UI_WHITE);
    }

    /* Draw empty space */
    uint8_t empty_pipes = (uint8_t)(8U - pipes);
    for (uint8_t i = 0U; i < empty_pipes; i++) {
        hal_ui_text((uint8_t)(pipe_x + ((pipes + i) * 6U)), y, " ", HAL_UI_WHITE);
    }

    /* Close bracket and percentage */
    uint8_t close_x = (uint8_t)(pipe_x + (8U * 6U));
    hal_ui_text(close_x, y, "]", HAL_UI_WHITE);

    if (value_text != NULL) {
        uint8_t pct_x = (uint8_t)(close_x + 8U);
        hal_ui_text(pct_x, y, value_text, HAL_UI_WHITE);
    }
}

static CeePewErr_t render_diag_page(void)
{
    CEEPEW_ASSERT(s_ui_manager_initialised, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(g_ui_ctx.diag_mode, CEEPEW_ERR_PARAM);

    hal_ui_clear();

    DiagMetrics_t metrics;
    diag_collect_metrics(&metrics);

    uint8_t page = ui_map_pot_to_index(g_ui_ctx.user_input, s_diag_page_count);
    char ln[48U];
    diag_draw_header(s_diag_page_names[page], page);

    switch (page) {
        case 0U: {
            /* Page 1: CPU, HEAP, PSRAM with inline HTOP-style bars */
            char cpu_val[12U];
            char heap_val[18U];
            char psram_val[18U];
            (void)snprintf(cpu_val, sizeof(cpu_val), "%u%%", (unsigned)metrics.cpu_load_pct);
            (void)snprintf(heap_val, sizeof(heap_val), "%luK",
                           (unsigned long)(metrics.heap_used_bytes / 1024U));
            if (metrics.psram_total_bytes > 0U) {
                (void)snprintf(psram_val, sizeof(psram_val), "%luK",
                               (unsigned long)(metrics.psram_used_bytes / 1024U));
            } else {
                (void)snprintf(psram_val, sizeof(psram_val), "n/a");
            }
            diag_draw_metric_bar(0U, 14U, 60U, "CPU", metrics.cpu_load_pct, cpu_val);
            diag_draw_metric_bar(0U, 24U, 60U, "HEAP",
                                 (metrics.heap_total_bytes > 0U)
                                     ? (uint8_t)((metrics.heap_used_bytes * 100U) / metrics.heap_total_bytes)
                                     : 0U,
                                 heap_val);
            diag_draw_metric_bar(0U, 34U, 60U, "PSRAM",
                                 (metrics.psram_total_bytes > 0U)
                                     ? (uint8_t)((metrics.psram_used_bytes * 100U) / metrics.psram_total_bytes)
                                     : 0U,
                                 psram_val);
            (void)snprintf(ln, sizeof(ln), "Uptime:%lus", (unsigned long)metrics.uptime_seconds);
            hal_ui_text(0U, 44U, ln, HAL_UI_WHITE);
            (void)snprintf(ln, sizeof(ln), "Draw:%luus Hz:%lu",
                           (unsigned long)metrics.draw_cost_us,
                           (unsigned long)metrics.loop_rate_hz);
            hal_ui_text(0U, 52U, ln, HAL_UI_WHITE);
        } break;

        case 1U: {
            /* Page 2: Task/process information */
            uint32_t task_count = uxTaskGetNumberOfTasks();
            uint32_t heap_free_k = metrics.heap_free_bytes / 1024U;

            (void)snprintf(ln, sizeof(ln), "Tasks:%lu", (unsigned long)task_count);
            hal_ui_text(0U, 14U, ln, HAL_UI_WHITE);

            (void)snprintf(ln, sizeof(ln), "FreeHeap:%luK", (unsigned long)heap_free_k);
            hal_ui_text(0U, 22U, ln, HAL_UI_WHITE);

            (void)snprintf(ln, sizeof(ln), "StackHWM:%luW", (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            hal_ui_text(0U, 30U, ln, HAL_UI_WHITE);

            /* Show CPU load details */
            uint32_t period_us = CEEPEW_UI_LOOP_DELAY_MS * 1000U;
            if (period_us > 0U) {
                uint32_t busy_us = metrics.draw_cost_us;
                (void)snprintf(ln, sizeof(ln), "UI Draw:%luus/%luus",
                               (unsigned long)busy_us,
                               (unsigned long)period_us);
                hal_ui_text(0U, 38U, ln, HAL_UI_WHITE);
            }

            /* Session info */
            bool sess_active = session_is_active();
            BleState_t ble_state = transport_ble_get_state();
            (void)snprintf(ln, sizeof(ln), "Sess:%s BLE:%s",
                           sess_active ? "Y" : "N",
                           diag_ble_state_name(ble_state));
            hal_ui_text(0U, 46U, ln, HAL_UI_WHITE);

            /* Peer info if available */
            const BlePeerRecord_t *peer = transport_ble_get_peer();
            if (peer != NULL) {
                (void)snprintf(ln, sizeof(ln), "Peer:%02X%02X%02X",
                               peer->peer_mac[3], peer->peer_mac[4],
                               peer->peer_mac[5]);
                hal_ui_text(0U, 54U, ln, HAL_UI_WHITE);
            }
        } break;

        case 2U: {
            /* Page 3: Memory and storage visualization */
            uint32_t heap_total_k = metrics.heap_total_bytes / 1024U;
            uint32_t heap_used_k = metrics.heap_used_bytes / 1024U;
            uint32_t heap_free_k = metrics.heap_free_bytes / 1024U;
            uint8_t heap_pct = (metrics.heap_total_bytes > 0U)
                             ? (uint8_t)((metrics.heap_used_bytes * 100U) / metrics.heap_total_bytes)
                             : 0U;

            /* Heap bar */
            diag_draw_metric_bar(0U, 14U, 60U, "Heap", heap_pct, "used");

            /* Memory details */
            (void)snprintf(ln, sizeof(ln), "Used:%luK Free:%luK", (unsigned long)heap_used_k, (unsigned long)heap_free_k);
            hal_ui_text(0U, 24U, ln, HAL_UI_WHITE);

            (void)snprintf(ln, sizeof(ln), "Total:%luK", (unsigned long)heap_total_k);
            hal_ui_text(0U, 32U, ln, HAL_UI_WHITE);

            /* PSRAM if available - text summary only, no duplicate bar */
            if (metrics.psram_total_bytes > 0U) {
                uint32_t psram_used_k = metrics.psram_used_bytes / 1024U;
                uint32_t psram_total_k = metrics.psram_total_bytes / 1024U;
                (void)snprintf(ln, sizeof(ln), "PSRAM:%luK/%luK", (unsigned long)psram_used_k, (unsigned long)psram_total_k);
                hal_ui_text(0U, 44U, ln, HAL_UI_WHITE);
            } else {
                (void)snprintf(ln, sizeof(ln), "PSRAM: Not available");
                hal_ui_text(0U, 44U, ln, HAL_UI_WHITE);
            }
        } break;

        case 3U: {
            /* Page 4: Runtime information (unchanged) */
            wifi_mode_t wifi_mode = WIFI_MODE_NULL;
            wifi_ap_record_t ap_info;
            bool wifi_connected = false;
            bool wifi_enabled = false;
            if (esp_wifi_get_mode(&wifi_mode) == ESP_OK) {
                wifi_enabled = (wifi_mode != WIFI_MODE_NULL);
            }
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                wifi_connected = true;
            }

            esp_chip_info_t chip_info;
            esp_chip_info(&chip_info);
            esp_reset_reason_t reset_reason = esp_reset_reason();

            (void)snprintf(ln, sizeof(ln), "BLE:%s  Active:%s",
                           diag_ble_state_name(transport_ble_get_state()),
                           session_is_active() ? "Y" : "N");
            hal_ui_text(0U, 14U, ln, HAL_UI_WHITE);

            const BlePeerRecord_t *peer = transport_ble_get_peer();
            if (peer != NULL) {
                (void)snprintf(ln, sizeof(ln), "Peer:%02X%02X%02X RSSI:%d",
                               peer->peer_mac[3], peer->peer_mac[4],
                               peer->peer_mac[5], (int)g_ble_ctx.peer_rssi);
            } else {
                (void)snprintf(ln, sizeof(ln), "Peer:none");
            }
            hal_ui_text(0U, 22U, ln, HAL_UI_WHITE);

            (void)snprintf(ln, sizeof(ln), "WiFi:%s  Chip:%uc Rev%u",
                           wifi_enabled ? (wifi_connected ? "conn" : "on") : "off",
                           (unsigned)chip_info.cores,
                           (unsigned)chip_info.revision);
            hal_ui_text(0U, 30U, ln, HAL_UI_WHITE);

            (void)snprintf(ln, sizeof(ln), "Uptime:%lus  Rst:%d",
                           (unsigned long)metrics.uptime_seconds,
                           (int)reset_reason);
            hal_ui_text(0U, 38U, ln, HAL_UI_WHITE);

            (void)snprintf(ln, sizeof(ln), "Scan:%lu Hits:%u",
                           (unsigned long)g_ble_ctx.scan_seen_count,
                           (unsigned)g_ble_ctx.scan_hit_count);
            hal_ui_text(0U, 46U, ln, HAL_UI_WHITE);
        } break;

        default:
            break;
    }

    return CEEPEW_OK;
}

CeePewErr_t ui_manager_draw(void)
{
    CEEPEW_ASSERT(s_ui_manager_initialised, CEEPEW_ERR_PARAM);
    uint64_t draw_start_us = (uint64_t)esp_timer_get_time();

    /* ── DIAG MODE OVERRIDE ────────────────────────────────────────────
     * The push-lock switch on GPIO 5 (active LOW, CEEPEW_DIAG_SWITCH_ACTIVE)
     * sets g_ui_ctx.diag_mode. When active we always render DIAG, bypassing
     * all normal screen logic. Session FSM on Core 1 is unaffected.
     * ─────────────────────────────────────────────────────────────────── */
    if (g_ui_ctx.diag_mode) {
        CeePewErr_t diag_err = render_diag_page();
        if (diag_err != CEEPEW_OK) { return diag_err; }
        return hal_ui_flush();
    }

    /* Normal screen dispatch */
    CeePewErr_t err = CEEPEW_OK;

    switch (g_ui_ctx.current_state) {
        case UI_STATE_BOOT:               err = render_boot_anim();          break;
        case UI_STATE_DISCOVERY:          err = render_discovery();           break;
        case UI_STATE_CODE_ENTRY:         err = render_code_entry();          break;
        case UI_STATE_COUNTDOWN:          err = render_syncing();             break;
        case UI_STATE_PAIRING:            err = render_countdown();           break;
        case UI_STATE_CONFIRM:            err = render_confirm();             break;
        case UI_STATE_PAIRING_SUCCESS:    err = render_pairing_success();     break;
        case UI_STATE_PAIRING_FAILED:     err = render_pairing_failed();      break;
        case UI_STATE_KEYDER:             err = render_keyder_anim();         break;
        case UI_STATE_FINGERPRINT:        err = render_fingerprint();         break;
        case UI_STATE_FINGERPRINT_CONFIRM:err = render_fingerprint_confirm(); break;
        case UI_STATE_CHAT:               err = render_chat_thread();         break;
        case UI_STATE_CHAT_MENU:          err = render_chat_menu();           break;
        case UI_STATE_CHAT_COMPOSE:       err = render_chat_compose();        break;
        case UI_STATE_CHAT_SEND_CONFIRM:  err = render_chat_send_confirm();   break;
        case UI_STATE_CRYPTOGRAM:         err = render_cryptogram();          break;
        case UI_STATE_NONCE_EXHAUSTED:    err = ui_show_nonce_exhausted();    break;
        case UI_STATE_CODE_INCORRECT:     err = render_code_incorrect();      break;
        case UI_STATE_CODE_DIFFERENT:     err = render_code_different();      break;
        case UI_STATE_INFO:               err = render_info();                break;
        case UI_STATE_ERROR:              err = render_error();               break;
        default:                          return CEEPEW_ERR_PARAM;
    }

    if (err != CEEPEW_OK) { return err; }
    err = hal_ui_flush();

    uint64_t draw_end_us = (uint64_t)esp_timer_get_time();
    if (s_diag_prev_draw_start_us != 0ULL && draw_start_us > s_diag_prev_draw_start_us) {
        uint64_t delta_us = draw_start_us - s_diag_prev_draw_start_us;
        if (delta_us > 0ULL) {
            s_diag_last_loop_rate_hz = (uint32_t)(1000000ULL / delta_us);
        }
    }
    s_diag_prev_draw_start_us = draw_start_us;
    s_diag_last_draw_cost_us = (draw_end_us > draw_start_us)
                             ? (uint32_t)(draw_end_us - draw_start_us)
                             : 0U;

    return err;
}

CeePewErr_t ui_manager_transition_to(UIState_t next_state)
{
    CEEPEW_ASSERT(next_state <= UI_STATE_CHAT_SEND_CONFIRM, CEEPEW_ERR_PARAM);
    g_ui_ctx.next_state = next_state;
    return CEEPEW_OK;
}

CeePewErr_t ui_manager_handle_input(uint8_t pot_value, bool button_pressed, bool diag_mode)
{
    g_ui_ctx.user_input = pot_value;
    g_ui_ctx.button_pressed = button_pressed;
    g_ui_ctx.diag_mode = diag_mode;
    return CEEPEW_OK;
}

uint8_t ui_manager_get_anim_frame(void)
{
    return (uint8_t)(g_ui_ctx.anim.frame_count & 0xFFU);
}

/* ────────────────────────────────────────────────────────────────────── */
/* Phase 4: Fingerprint Confirmation and Nonce Exhausted Screens */
/* ────────────────────────────────────────────────────────────────────── */

/* Phase 4: Fingerprint confirmation panel.
 * Displays 16-byte fingerprint in hex, peer MAC, and D=Accept/S=Reject prompts.
 */
CeePewErr_t ui_fingerprint_show_confirm(const uint8_t fingerprint[16],
                                        const uint8_t peer_mac[6])
{
    CEEPEW_ASSERT(fingerprint != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);

    hal_ui_clear();

    /* Title */
    hal_ui_text(0U, 0U, "FINGERPRINT", HAL_UI_WHITE);

    /* Fingerprint hex grouped into 2-byte blocks for readability */
    ui_draw_hex_rows(fingerprint, 16U, 0U, 14U);

    /* Line 3: Peer MAC */
    char peer_str[14];
    (void)snprintf(peer_str, sizeof(peer_str), "Peer: %02X%02X",
                   peer_mac[4U], peer_mac[5U]);
    hal_ui_text(0U, 36U, peer_str, HAL_UI_WHITE);

    /* Line 4: Prompt */
    hal_ui_text(0U, 48U, "D=Yes  S=No", HAL_UI_WHITE);

    /* flush handled by ui_manager_draw() */
    return CEEPEW_OK;
}

CeePewErr_t ui_fingerprint_show_display(const uint8_t fingerprint[16],
                                        const uint8_t peer_mac[6])
{
    CEEPEW_ASSERT(fingerprint != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);

    hal_ui_clear();
    hal_ui_text(0U, 0U, "PEER FINGERPRINT", HAL_UI_WHITE);

    ui_draw_hex_rows(fingerprint, 16U, 0U, 16U);

    char peer_str[20];
    (void)snprintf(peer_str, sizeof(peer_str), "Peer %02X%02X", peer_mac[4U], peer_mac[5U]);
    hal_ui_text(0U, 40U, peer_str, HAL_UI_WHITE);
    hal_ui_text(0U, 52U, "Press to confirm", HAL_UI_WHITE);

    /* flush handled by ui_manager_draw() */
    return CEEPEW_OK;
}

/* Phase 4: Nonce exhausted error panel.
 * Displays "Session expired — nonce limit hit. Restart to re-pair."
 * with red blink animation.
 */
CeePewErr_t ui_show_nonce_exhausted(void)
{
    hal_ui_clear();

    /* Title */
    hal_ui_text(0U, 0U, "NONCE EXHAUSTED", HAL_UI_WHITE);

    /* Error message (multi-line) */
    hal_ui_text(0U, 16U, "Session expired.", HAL_UI_WHITE);
    hal_ui_text(0U, 26U, "Nonce limit hit.", HAL_UI_WHITE);
    hal_ui_text(0U, 36U, "Restart to pair.", HAL_UI_WHITE);

    /* Blinking indicator */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    uint32_t elapsed_ms = now_ms - g_ui_ctx.error_start_ms;
    uint8_t blink_phase = (uint8_t)((elapsed_ms / CEEPEW_RGB_ERROR_BLINK_MS) % 2U);

    if (blink_phase == 0U) {
        HalUIRect_t blink_box = {.x = 62U, .y = 52U, .w = 4U, .h = 4U};
        hal_ui_rect_fill(&blink_box, HAL_UI_WHITE);
    }

    /* flush handled by ui_manager_draw() */
    return CEEPEW_OK;
}

static CeePewErr_t render_error(void)
{
    hal_ui_clear();
    bool send_fail = (g_ui_ctx.reject_sequence_start_ms == 0U);
    hal_ui_text(16U, 8U, send_fail ? "SEND FAILED" : "SECURITY RESET", HAL_UI_WHITE);
    hal_ui_text(8U, 22U, send_fail ? "Could not send" : "Fingerprint rejected", HAL_UI_WHITE);
    hal_ui_text(8U, 34U, send_fail ? "Draft kept" : "Wiping session...", HAL_UI_WHITE);
    if (send_fail) {
        hal_ui_text(4U, 46U, "Tap=retry  Hold=menu", HAL_UI_WHITE);
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    uint32_t start_ms = send_fail ? g_ui_ctx.error_start_ms : g_ui_ctx.reject_sequence_start_ms;
    uint32_t elapsed_ms = (now_ms >= start_ms) ? (now_ms - start_ms) : 0U;
    uint8_t blink_phase = (uint8_t)((elapsed_ms / CEEPEW_RGB_ERROR_BLINK_MS) % 2U);
    if (blink_phase == 0U) {
        HalUIRect_t blink_box = {.x = 56U, .y = 52U, .w = 16U, .h = 4U};
        hal_ui_rect_fill(&blink_box, HAL_UI_WHITE);
    }

    /* flush handled by ui_manager_draw() */
    return CEEPEW_OK;
}

/* Phase 4: Reset UI to discovery mode.
 * Called by session_wipe() to hard-reset UI on TTL expiry or security event.
 * Clears OLED, sets state to discovery, and readies RGB for discovery pattern.
 */
void ui_manager_reset_to_discovery(void)
{
    if (!s_ui_manager_initialised) { return; }

    hal_ui_clear();
    hal_ui_flush();

    g_ui_ctx.current_state = UI_STATE_DISCOVERY;
    g_ui_ctx.next_state = UI_STATE_DISCOVERY;
    g_ui_ctx.anim.frame_count = 0U;
    g_ui_ctx.anim.active = true;
    g_ui_ctx.transition_ready = false;
    g_ui_ctx.button_pressed = false;
    g_ui_ctx.button_prev = false;
    g_ui_ctx.fingerprint_confirmed = false;
    g_ui_ctx.reject_sequence_start_ms = 0U;
    g_ui_ctx.error_start_ms = 0U;
    g_ui_ctx.pairing_result_start_ms = 0U;
    g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_NONE;
    g_ui_ctx.crypto_confirm_start_ms = 0U;
    memset(g_ui_ctx.fingerprint, 0U, sizeof(g_ui_ctx.fingerprint));
    memset(g_ui_ctx.peer_mac, 0U, sizeof(g_ui_ctx.peer_mac));

    BleState_t ble_state = transport_ble_get_state();
    if (ble_state == BLE_DONE) {
        (void)transport_ble_disconnect();
        ble_state = transport_ble_get_state();
    }
    if (ble_state == BLE_IDLE) {
        (void)transport_ble_start_advertising();
    }
}

/* components/ceepew_hal/ui_manager.c
 * UI Manager Implementation — handles all remaining screen states and transitions.
 */

#include "ui_manager.h"
#include "hal_ui.h"
#include "../transport/transport_ble.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "../../main/session_fsm.h"
#include "../../main/session_msgstore.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

/* Design note: The UI manager is a simple state machine that tracks the
   current screen, animation state, and user input. Each screen state calls
   its corresponding render function. Transitions between screens are driven
   by completion flags and user input. This architecture allows all 9 screen
   types to coexist in one efficient state machine. */

/* Weak reference to BLE context; actual definition lives in transport_ble.c
   but a weak symbol here allows building/linking in configurations where the
   transport component may be omitted during unit tests or static linking. */
__attribute__((weak)) BleContext_t g_ble_ctx = {0};

/* Forward declarations of session FSM accessors for Sprint 10 integration */
extern uint64_t session_get_id(void);
extern uint64_t session_get_nonce_counter(void);
extern CeePewErr_t session_get_commitment(uint8_t commitment[8]);
extern CeePewErr_t session_get_fingerprint(uint8_t fingerprint[16]);

UIContext_t g_ui_ctx = {0};
static bool s_ui_manager_initialised = false;

static CeePewErr_t render_fingerprint_confirm(void);
static CeePewErr_t render_error(void);
static CeePewErr_t render_code_incorrect(void);
static CeePewErr_t render_code_different(void);

CeePewErr_t ui_manager_init(void){
    CEEPEW_ASSERT(!s_ui_manager_initialised, CEEPEW_ERR_BUSY);
    memset(&g_ui_ctx, 0U, sizeof(UIContext_t));
    g_ui_ctx.current_state = UI_STATE_BOOT;
    g_ui_ctx.next_state = UI_STATE_BOOT;
    g_ui_ctx.anim.frame_count = 0U;
    g_ui_ctx.anim.frame_rate_ms = 50U;  /* 20 FPS */
    g_ui_ctx.anim.active = true;
    g_ui_ctx.last_draw_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    g_ui_ctx.user_input = 0U;
    g_ui_ctx.button_pressed = false;
    g_ui_ctx.diag_mode = false;
    g_ui_ctx.transition_ready = false;
    g_ui_ctx.code_entry_start_ms = 0U;
    s_ui_manager_initialised = true;
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

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1b — SELF-CONTAINED 5×7 FONT
 *
 * Bypasses hal_ui_text() which has no working font data.
 * Each character: 5 bytes = 5 columns. Each byte = 1 column, 7 rows.
 * bit 0 = topmost pixel row, bit 6 = bottommost.
 * Characters encoded for ASCII 0x20 (space) through 0x7E (~).
 * ═══════════════════════════════════════════════════════════════════════════ */

static const uint8_t s_font5x7[95][5] = {
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

#undef  hal_ui_rect
static void draw_rect_outline(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
#define hal_ui_rect(r, colour)  draw_rect_outline((r)->x, (r)->y, (r)->w, (r)->h)

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

       /* Underline grows from left under the logo */
       if (f >= 58U) {
           uint32_t ul_f = f - 58U;
           uint8_t  ul_w = (ul_f < 20U) ? (uint8_t)(ul_f * 3U) : 59U;
           if (ul_w > 0U) { draw_hline(43U, 23U, ul_w); }
       }
    }

    /* ── Phase 3 (f 60–89): Chunky loading bar fills ── */
    if (f >= 60U) {
       /* Bar border */
       HalUIRect_t border = { .x = 14U, .y = 44U, .w = 100U, .h = 10U };
       hal_ui_rect(&border, HAL_UI_WHITE);

       /* Chunky fill — 4-px wide segments with 1-px gaps */
       uint32_t elapsed = f - 60U;
       uint8_t  seg_count = (elapsed < 30U) ? (uint8_t)((elapsed * 12U) / 30U) : 12U;
       for (uint8_t s = 0U; s < seg_count; s++) {
           HalUIRect_t seg = { .x = (uint8_t)(16U + s * 8U), .y = 46U, .w = 7U, .h = 6U };
           hal_ui_rect_fill(&seg, HAL_UI_WHITE);
       }

       /* Percentage counter — moved BELOW bar to avoid overlap */
       uint8_t pct = (seg_count * 100U) / 12U;
       char pct_str[5U];
       (void)snprintf(pct_str, sizeof(pct_str), "%3u%%", (unsigned int)pct);
       hal_ui_text(50U, 56U, pct_str, HAL_UI_WHITE);
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
           hal_ui_text(16U, 30U, "SECURE MESSENGER", HAL_UI_WHITE);
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
    /* Trail 4 (faintest, oldest): outer ring only */
    uint8_t t4 = (pos + 12U) % 16U;
    draw_radial_segment(RDR_CX, RDR_CY,
                        SWEEP16[t4].ex, SWEEP16[t4].ey,
                        RDR_R3 - 4U, RDR_R3);

    /* Trail 3: arm to 45% of outer radius */
    uint8_t t3 = (pos + 13U) % 16U;
    draw_radial_segment(RDR_CX, RDR_CY,
                        SWEEP16[t3].ex, SWEEP16[t3].ey,
                        RDR_R2 + 2U, RDR_R3);

    /* Trail 2: arm to 65% */
    uint8_t t2 = (pos + 14U) % 16U;
    draw_radial_segment(RDR_CX, RDR_CY,
                        SWEEP16[t2].ex, SWEEP16[t2].ey,
                        RDR_R1 + 4U, RDR_R3);

    /* Trail 1: arm to 85% */
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

/* Peer blip renderer.
 * The blip only becomes visible when the sweep arm is near the derived
 * peer angle, which prevents the "fixed GIF" look from a static marker. */
static void draw_peer_blip(uint8_t bx, uint8_t by,
                           uint8_t sweep_pos, uint8_t blip_idx,
                           uint32_t age_ms)
{
    if (age_ms >= 4000U) {
        return;
    }

    draw_pixel(bx, by);

    uint8_t forward = (uint8_t)((blip_idx + 16U - sweep_pos) % 16U);
    uint8_t reverse = (uint8_t)((sweep_pos + 16U - blip_idx) % 16U);
    uint8_t dist = (forward < reverse) ? forward : reverse;
    if (dist == 0U) {
        HalUIRect_t blip = {
            .x = (uint8_t)(bx - 1U),
            .y = (uint8_t)(by - 1U),
            .w = 3U,
            .h = 3U
        };
        hal_ui_rect_fill(&blip, HAL_UI_WHITE);
        if (age_ms < 400U) {
            draw_circle((int16_t)bx, (int16_t)by, 3U);
        }
    } else if (dist == 1U) {
        HalUIRect_t blip = {
            .x = (uint8_t)(bx - 1U),
            .y = (uint8_t)(by - 1U),
            .w = 2U,
            .h = 2U
        };
        hal_ui_rect_fill(&blip, HAL_UI_WHITE);
    } else {
        /* Keep a stable single-pixel blip visible even between sweep hits. */
        draw_pixel(bx, by);
    }
}

static void draw_rssi_bars(uint8_t x, uint8_t y, uint8_t bars)
{
    for (uint8_t b = 0U; b < 5U; b++) {
        uint8_t bx  = (uint8_t)(x + b * 6U);
        uint8_t bh  = (uint8_t)(2U + b * 2U);
        uint8_t by  = (uint8_t)(y + 10U - bh);
        HalUIRect_t bar = { .x = bx, .y = by, .w = 5U, .h = bh };
        if (b < bars) {
            hal_ui_rect_fill(&bar, HAL_UI_WHITE);
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
    #define Y_PEER_ID       10U     /* Peer info section (reduced gap from 12) */
    #define Y_RSSI_BARS     18U     /* RSSI signal bars */
    #define Y_RSSI_DBM      30U     /* dBm value */
    #define Y_PAIR_BTN      39U     /* PAIR button */
    #define Y_BLE_STATE     48U     /* BLE: XXXX state */
    #define Y_STATUS        56U     /* Status line */

    /* ── Left panel: Radar ── */
    draw_radar_static();

    uint8_t sweep_pos = (uint8_t)((now_ms / 150U) % 16U);
    draw_sweep_arm(sweep_pos);

    /* ── Right panel: header + status ── */
    /* Use a shared raw scroll counter; ui_draw_rotating_text() wraps per string
     * length so each label loops seamlessly without a hardcoded cycle. */
    uint32_t shared_scroll_px = (g_ui_ctx.anim.frame_count / 3U);

    /* Fixed title to keep header stable */
    hal_ui_text(RPANEL_X, Y_TITLE, "DISCOVERING PEERS", HAL_UI_WHITE);

    /* ── Peer discovery feedback ── */
    if (g_ble_ctx.discovered) {
       /* Peer found: show MAC, RSSI, pair prompt */
       char peer_id[10U];
       (void)snprintf(peer_id, sizeof(peer_id), "ID:%02X%02X%02X",
                      g_ble_ctx.peer_mac[3], g_ble_ctx.peer_mac[4],
                      g_ble_ctx.peer_mac[5]);
       hal_ui_text(RPANEL_X, Y_PEER_ID, peer_id, HAL_UI_WHITE);

       int16_t rssi_smooth = (int16_t)(g_ble_ctx.peer_rssi_smooth_x8 / 8);
       if (rssi_smooth < -90) { rssi_smooth = -90; }
       if (rssi_smooth > -30) { rssi_smooth = -30; }

       uint8_t bars = rssi_to_bars((int8_t)rssi_smooth);
       draw_rssi_bars(RPANEL_X, Y_RSSI_BARS, bars);

       char rssi_str[10U];
       (void)snprintf(rssi_str, sizeof(rssi_str), "%ddBm", (int)rssi_smooth);
       hal_ui_text(RPANEL_X, Y_RSSI_DBM, rssi_str, HAL_UI_WHITE);

       /* Prompt user to press the button to begin pairing (only if not already confirmed) */
       if (!g_ble_ctx.commitment_verified && !transport_ble_handoff_ready()) {
           hal_ui_text(RPANEL_X, Y_PAIR_BTN, "Press button to pair", HAL_UI_WHITE);
       }

       /* Blip rendering — only visible near sweep arm */
       uint8_t blip_r = (uint8_t)(
           (((int16_t)(-rssi_smooth - 30) * (int16_t)(RDR_R3 - 3U)) / 60) + 2U);

       uint8_t base_idx = (uint8_t)(g_ble_ctx.peer_mac[5] % 16U);
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
       if (bx >= 0 && bx < 52 && by >= 10 && by < 64) {
           uint32_t age_ms = (g_ble_ctx.last_seen_ms == 0U)
                           ? 0U : (now_ms - g_ble_ctx.last_seen_ms);
           draw_peer_blip((uint8_t)bx, (uint8_t)by, sweep_pos, blip_idx, age_ms);
       }
    } else {
       /* Peer not yet found: show scanning prompt (synchronized with title above) */
       ui_draw_rotating_text(RPANEL_X, Y_PEER_ID, "Awaiting peer...", RPANEL_WIDTH, shared_scroll_px);
    }

    /* ── BLE state indicator ── */
    const char *ble_state = "BLE: IDLE";
    switch (transport_ble_get_state()) {
       case BLE_IDLE:        ble_state = "BLE: IDLE"; break;
       case BLE_ADVERTISING: ble_state = "BLE: ADV";  break;
       case BLE_SCANNING:    ble_state = "BLE: SCAN"; break;
       case BLE_ADVERTISING_AND_SCANNING: ble_state = "BLE: ADV+SCN"; break;
       case BLE_CONNECTED:   ble_state = "BLE: CONN"; break;
       case BLE_PAIRING:     ble_state = "BLE: PAIR"; break;
       case BLE_DONE:        ble_state = "BLE: DONE"; break;
       default:              ble_state = "BLE: ?";    break;
    }
    hal_ui_text(RPANEL_X, Y_BLE_STATE, ble_state, HAL_UI_WHITE);

    /* ── Bottom status line ── */
    if (g_ble_ctx.discovered) {
       uint32_t age_ms = (g_ble_ctx.last_seen_ms == 0U)
                       ? 0U : (now_ms - g_ble_ctx.last_seen_ms);
       char status_str[28U];
       (void)snprintf(status_str, sizeof(status_str), "S:%lu H:%u A:%lus",
                      (unsigned long)g_ble_ctx.scan_seen_count,
                      (unsigned)g_ble_ctx.scan_hit_count,
                      (unsigned long)(age_ms / 1000U));
       hal_ui_text(RPANEL_X, Y_STATUS, status_str, HAL_UI_WHITE);
    } else {
       /* Scanning: show activity summary */
       if (transport_ble_get_state() == BLE_ADVERTISING ||
           transport_ble_get_state() == BLE_SCANNING ||
           transport_ble_get_state() == BLE_ADVERTISING_AND_SCANNING) {
           char adv_str[20U];
           (void)snprintf(adv_str, sizeof(adv_str), "SCAN S:%lu",
                          (unsigned long)g_ble_ctx.scan_seen_count);
           hal_ui_text(RPANEL_X, Y_STATUS, adv_str, HAL_UI_WHITE);
       } else {
           static const char *const SCAN_STATES[4] = { "SCAN", "SCAN.", "SCAN..", "SCAN..." };
           uint8_t dot_idx = (uint8_t)((now_ms / 500U) % 4U);
           hal_ui_text(RPANEL_X, Y_STATUS, SCAN_STATES[dot_idx], HAL_UI_WHITE);
       }
    }

    return CEEPEW_OK;
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

    /* Four digit cells — each 24px wide, 22px tall */
    /* Cells at x: 4, 30, 56, 82 (centred across 128px) */
    const uint8_t cell_x[4U] = { 8U, 34U, 60U, 86U };
    const uint8_t cell_w     = 24U;
    const uint8_t cell_h     = 28U;
    const uint8_t cell_y     = 18U;
    
    for (uint8_t i = 0U; i < 4U; i++) {
        bool is_active = (g_ui_ctx.code_selected == i);

        if (is_active) {
            /* Active cell: double border blinks at ~4Hz */
            uint8_t blink = (uint8_t)((f / 8U) % 2U);
            HalUIRect_t outer = { .x = cell_x[i] - 2U, .y = cell_y - 2U,
                                  .w = cell_w + 4U, .h = cell_h + 4U };
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
            hal_ui_text(cell_x[i] + 9U, cell_y + 10U, digit_str, HAL_UI_WHITE);
        }
    }

    /* Index indicator dots below cells */
    for (uint8_t i = 0U; i < 4U; i++) {
        uint8_t dot_x = (uint8_t)(cell_x[i] + cell_w / 2U);
        if (g_ui_ctx.code_selected == i) {
            HalUIRect_t dot = { .x = dot_x - 2U, .y = 50U, .w = 5U, .h = 5U };
            hal_ui_rect_fill(&dot, HAL_UI_WHITE);
        } else if (i < g_ui_ctx.code_selected) {
            HalUIRect_t dot = { .x = dot_x - 1U, .y = 51U, .w = 3U, .h = 3U };
            hal_ui_rect_fill(&dot, HAL_UI_WHITE);
        } else {
            draw_pixel(dot_x, 52U);
        }
    }

    /* Instructions */
    hal_ui_text(4U, 56U, "Turn pot / press", HAL_UI_WHITE);

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

static CeePewErr_t render_countdown(void)
{
    hal_ui_clear();

    uint32_t f    = g_ui_ctx.anim.frame_count;
    uint32_t now  = (uint32_t)(esp_timer_get_time() / 1000LL);
    uint32_t elapsed = (now >= g_ui_ctx.countdown_start_ms)
                     ? (now - g_ui_ctx.countdown_start_ms) : 0U;
    uint32_t total_ms = (uint32_t)CEEPEW_PAIRING_TIMEOUT_S * 1000U;
    uint32_t rem_ms   = (elapsed < total_ms) ? (total_ms - elapsed) : 0U;
    uint8_t  secs     = (uint8_t)(rem_ms / 1000U);

    /* Title */
    hal_ui_text(30U, 2U, "SYNCING", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    /* Large countdown number — left-aligned to leave room for the sync pulse */
    char sec_str[4U];
    (void)snprintf(sec_str, sizeof(sec_str), "%2u", (unsigned)secs);
    hal_ui_text(14U, 20U, "Syncing in", HAL_UI_WHITE);
    hal_ui_text(14U, 30U, sec_str, HAL_UI_WHITE);
    hal_ui_text(42U, 30U, "sec", HAL_UI_WHITE);

    /* Animated progress bar — separate from the sync pulse */
    uint32_t fill = (rem_ms * 110U) / total_ms;  /* 0..110 */
    if (fill > 110U) { fill = 110U; }

    /* Bar border */
    HalUIRect_t border = { .x = 9U, .y = 40U, .w = 110U, .h = 10U };
    hal_ui_rect(&border, HAL_UI_WHITE);

    if (fill > 0U) {
        /* Pulsing fill — brightest at right edge */
        HalUIRect_t bar = { .x = 11U, .y = 42U, .w = (uint8_t)fill, .h = 6U };
        hal_ui_rect_fill(&bar, HAL_UI_WHITE);

        /* Bright leading edge 2px wide */
        uint8_t edge_x = (uint8_t)(11U + fill);
        if (edge_x < 120U) {
            draw_vline(edge_x, 41U, 8U);
        }
    }

    /* Pulsing ring on the right side so it never overlaps the timer text */
    uint8_t ring_r = (uint8_t)((f % 20U));
    if (ring_r > 0U && ring_r < 12U) {
        draw_circle(98, 24, (uint8_t)(ring_r / 2U + 2U));
    }

    /* "Waiting for peer" blink */
    uint8_t blink = (uint8_t)((f / 10U) % 2U);
    if (blink == 0U) {
        hal_ui_text(10U, 54U, "Waiting for peer...", HAL_UI_WHITE);
    }

    /* Tick marks at bar edges */
    draw_vline(9U,  40U, 4U);
    draw_vline(119U,40U, 4U);

    /* Countdown reached zero: decide next UI state based on session result */
    if (rem_ms == 0U) {
        uint8_t phase = session_get_phase();
        /* Require explicit handoff readiness (both peers verified) or an already-active session */
        if (session_is_active() || phase >= 3U || transport_ble_handoff_ready()) {
            /* Success: proceed to key derivation / fingerprint display */
            (void)ui_manager_transition_to(UI_STATE_KEYDER);
            g_ui_ctx.transition_ready = true;
        } else {
            /* Failure: show brief message and return to code entry to retry */
            hal_ui_text(10U, 54U, "Sync failed - retry", HAL_UI_WHITE);
            (void)ui_manager_transition_to(UI_STATE_CODE_ENTRY);
            g_ui_ctx.transition_ready = true;
        }
    }

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
    HalUIRect_t prompt = { .x = 14U, .y = 46U, .w = 100U, .h = 16U };
    if (blink == 0U) {
        hal_ui_rect(&prompt, HAL_UI_WHITE);
        hal_ui_text(18U, 51U, "HOLD BUTTON TO PAIR", HAL_UI_WHITE);
    } else {
        hal_ui_rect_fill(&prompt, HAL_UI_WHITE);
        /* Invert text — can't truly invert, so just skip text on filled frame */
    }

    g_ui_ctx.anim.frame_count++;
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
    HalUIRect_t pbar_bg = { .x = 0U, .y = 52U, .w = 128U, .h = 12U };
    hal_ui_rect_fill(&pbar_bg, HAL_UI_WHITE); /* white background strip */
    /* We need black fill inside — can't truly invert, so draw outline approach */
    HalUIRect_t pbar_border = { .x = 2U, .y = 54U, .w = 124U, .h = 8U };
    hal_ui_rect(&pbar_border, HAL_UI_WHITE);

    /* Derive animation progress from frame (assume ~150 frames for full derivation) */
    uint8_t prog_w = (uint8_t)((f < 150U) ? (f * 120U / 150U) : 120U);
    if (prog_w > 0U) {
        HalUIRect_t prog = { .x = 4U, .y = 56U, .w = prog_w, .h = 4U };
        hal_ui_rect_fill(&prog, HAL_UI_WHITE);
    }

    /* Label inside the progress bar white band */
    hal_ui_text(30U, 54U, "DERIVING KEY...", HAL_UI_WHITE);

    g_ui_ctx.anim.frame_count++;
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
    char fp_hex[33U];
    char row1[17U];
    char row2[17U];
    char mac_str[16U];
    uint8_t blink;

    hal_ui_text(22U, 2U, "VERIFY IDENTITY", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    /* Full fingerprint hex */
    for (uint8_t i = 0U; i < 16U; i++) {
        uint8_t  b = g_ui_ctx.fingerprint[i];
        uint8_t  h = (b >> 4U) & 0x0FU;
        uint8_t  l = b & 0x0FU;
        fp_hex[i * 2U]      = (h < 10U) ? (char)('0' + h) : (char)('A' + h - 10U);
        fp_hex[i * 2U + 1U] = (l < 10U) ? (char)('0' + l) : (char)('A' + l - 10U);
    }
    fp_hex[32U] = '\0';

    (void)memcpy(row1, fp_hex, 16U);
    row1[16U] = '\0';
    (void)memcpy(row2, fp_hex + 16U, 16U);
    row2[16U] = '\0';
    hal_ui_text(4U, 14U, row1, HAL_UI_WHITE);
    hal_ui_text(4U, 24U, row2, HAL_UI_WHITE);

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
    uint8_t bubble_x = (dir == 0U) ? 4U : 74U;
    uint8_t bubble_w = 50U;
    uint8_t text_x = (dir == 0U) ? 8U : 78U;

    /* Draw bubble outline */
    HalUIRect_t bubble = {.x = bubble_x, .y = y_pos, .w = bubble_w, .h = 10U};
    hal_ui_rect(&bubble, HAL_UI_WHITE);

    /* Display first 8 characters of message (or less if shorter) */
    char preview[9];
    uint8_t chars_to_show = (msg->meta.payload_len < 8U) ? msg->meta.payload_len : 8U;
    for (uint8_t i = 0U; i < chars_to_show; i++) {
        preview[i] = (char)((msg->encrypted[i] >= 32U && msg->encrypted[i] < 127U) ? msg->encrypted[i] : '?');
    }
    preview[chars_to_show] = '\0';

    hal_ui_text(text_x, (uint8_t)(y_pos + 2U), preview, HAL_UI_WHITE);

    /* Add status indicator (TTL countdown or delivery) */
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    uint32_t age_s = (now_s > msg->meta.created_at) ? (now_s - msg->meta.created_at) : 0U;
    uint32_t ttl_remaining = (age_s < CEEPEW_MSG_TTL_S) ? (CEEPEW_MSG_TTL_S - age_s) : 0U;

    /* Draw TTL indicator as small circle at top-right of bubble */
    if (ttl_remaining > 0U) {
        uint8_t status_x = (dir == 0U) ? 50U : 120U;
        hal_ui_circle(status_x, y_pos, 1U, HAL_UI_WHITE);
    }

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

    /* Character set: A-Z, 0-9, space, common punctuation */
    static const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,!?-";
    static const uint8_t CHARSET_LEN = 44U;  /* Exactly A-Z(26) + 0-9(10) + space(1) + punct(7) */

    /* Map pot value (0-255) to character index */
    uint8_t char_idx = (uint8_t)(((uint16_t)pot_value * CHARSET_LEN) / 256U);
    CEEPEW_ASSERT(char_idx < CHARSET_LEN, CEEPEW_ERR_BOUNDS);

    /* Display selector prompt */
    hal_ui_text(4U, 14U, "Select char:", HAL_UI_WHITE);

    /* Display current character selection (large) */
    char selected_char[2];
    selected_char[0] = CHARSET[char_idx];
    selected_char[1] = '\0';
    hal_ui_char(60U, 20U, selected_char[0], HAL_UI_WHITE);

    /* Draw selection highlight box */
    HalUIRect_t select_box = {.x = 54U, .y = 18U, .w = 20U, .h = 16U};
    hal_ui_rect(&select_box, HAL_UI_WHITE);

    /* Display instructions */
    hal_ui_text(4U, 38U, "Turn pot to select", HAL_UI_WHITE);
    hal_ui_text(8U, 46U, "Press to add char", HAL_UI_WHITE);

    return CEEPEW_OK;
}

static CeePewErr_t render_chat(void)
{
    hal_ui_clear();

    uint32_t f = g_ui_ctx.anim.frame_count;

    /* Status bar */
    hal_ui_text(0U, 0U, "SECURE CHAT", HAL_UI_WHITE);

    /* Heartbeat indicator — tiny pulsing dot at top-right */
    uint8_t hb = (uint8_t)((f / 15U) % 4U);
    if (hb < 2U) {
        HalUIRect_t hb_dot = { .x = 120U, .y = 1U, .w = 3U, .h = 3U };
        hal_ui_rect_fill(&hb_dot, HAL_UI_WHITE);
    }
    draw_hline(0U, 9U, 128U);

    /* Message area — placeholder text when no messages */
    uint8_t msg_count = msg_store_count();
    if (msg_count == 0U) {
        uint8_t blink = (uint8_t)((f / 20U) % 2U);
        if (blink == 0U) {
            hal_ui_text(12U, 24U, "No messages yet.", HAL_UI_WHITE);
            hal_ui_text(4U,  34U, "Press button to compose", HAL_UI_WHITE);
        }
    } else {
        /* Show up to 2 most recent message bubbles */
        uint8_t start = (msg_count > 2U) ? (uint8_t)(msg_count - 2U) : 0U;
        for (uint8_t i = start; i < msg_count && i < start + 2U; i++) {
            uint8_t slot = (uint8_t)(i - start);
            uint8_t bubble_y = (uint8_t)(12U + slot * 20U);
            (void)ui_chat_show_bubble(i, bubble_y, msg_store_get(i)->meta.dir);
        }
    }

    /* Compose mode triggered by button press */
    if (g_ui_ctx.button_pressed) {
        (void)ui_chat_show_compose(g_ui_ctx.user_input, 0U);
    }

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

/* Sprint 12: Helper function to display cryptogram with two hex rows.
 * Displays 8-byte commitment as 16 hex digits (8×2 layout).
 * Centered on display in monospace font.
 * No dynamic allocation; static hex conversion buffer.
 * Two CEEPEW_ASSERTs for bounds checking.
 */
CeePewErr_t ui_crypto_show_cryptogram(const uint8_t commitment[8])
{
    CEEPEW_ASSERT(commitment != NULL, CEEPEW_ERR_NULL_PTR);

    /* Convert 8 bytes to 16 hex characters */
    char hex_str[17];
    CEEPEW_ASSERT(sizeof(hex_str) >= 17U, CEEPEW_ERR_BOUNDS);

    /* Format as 16 hex digits */
    for (uint8_t i = 0U; i < 8U; i++) {
        uint8_t byte_val = commitment[i];
        uint8_t nibble_h = (byte_val >> 4U) & 0x0FU;
        uint8_t nibble_l = byte_val & 0x0FU;

        char hex_h = (nibble_h < 10U) ? (char)('0' + nibble_h) : (char)('A' + nibble_h - 10U);
        char hex_l = (nibble_l < 10U) ? (char)('0' + nibble_l) : (char)('A' + nibble_l - 10U);

        hex_str[i * 2U] = hex_h;
        hex_str[i * 2U + 1U] = hex_l;
    }
    hex_str[16] = '\0';

    /* Display title */
    hal_ui_text(20U, 8U, "Commitment", HAL_UI_WHITE);

    /* Display first 8 hex chars on line 1 (centered) */
    char line1[9];
    memcpy(line1, hex_str, 8U);
    line1[8] = '\0';
    hal_ui_text(20U, 22U, line1, HAL_UI_WHITE);

    /* Display second 8 hex chars on line 2 (centered) */
    char line2[9];
    memcpy(line2, hex_str + 8U, 8U);
    line2[8] = '\0';
    hal_ui_text(20U, 32U, line2, HAL_UI_WHITE);

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

    uint8_t y_pos = 46U;

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

    /* Build final hex string */
    char final_hex[17U];
    for (uint8_t i = 0U; i < 8U; i++) {
        uint8_t b = g_ui_ctx.commitment[i];
        uint8_t h = (b >> 4U) & 0x0FU;
        uint8_t l = b & 0x0FU;
        final_hex[i * 2U]      = (h < 10U) ? (char)('0' + h) : (char)('A' + h - 10U);
        final_hex[i * 2U + 1U] = (l < 10U) ? (char)('0' + l) : (char)('A' + l - 10U);
    }
    final_hex[16U] = '\0';

    /* Roll-up animation: characters count up for first 32 frames */
    char disp_hex[17U];
    for (uint8_t c = 0U; c < 16U; c++) {
        if (f < 32U) {
            /* Roll: start from '0', reach final in 32 frames, staggered by char */
            uint32_t char_f = (f > (uint32_t)c) ? (f - (uint32_t)c) : 0U;
            if (char_f >= 8U) {
                disp_hex[c] = final_hex[c];        /* settled */
            } else {
                /* Cycle through hex chars */
                uint8_t roll_idx = (uint8_t)(char_f * 2U % MATRIX_CHARSET_LEN);
                disp_hex[c] = MATRIX_CHARS[roll_idx];
            }
        } else {
            disp_hex[c] = final_hex[c];
        }
    }
    disp_hex[16U] = '\0';

    /* Title */
    hal_ui_text(20U, 2U, "COMMITMENT CODE", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    /* Two rows of 8 hex chars — centred */
    char line1[9U]; (void)memcpy(line1, disp_hex, 8U); line1[8U] = '\0';
    char line2[9U]; (void)memcpy(line2, disp_hex + 8U, 8U); line2[8U] = '\0';

    /* Decorative bracket around each row */
    hal_ui_text(10U, 16U, "[", HAL_UI_WHITE);
    hal_ui_text(18U, 16U, line1, HAL_UI_WHITE);
    hal_ui_text(66U, 16U, "]", HAL_UI_WHITE);

    hal_ui_text(10U, 26U, "[", HAL_UI_WHITE);
    hal_ui_text(18U, 26U, line2, HAL_UI_WHITE);
    hal_ui_text(66U, 26U, "]", HAL_UI_WHITE);

    /* Determine match status */
    uint8_t status = 0U;
    if (g_ble_ctx.commitment_verified) {
        uint8_t match = 1U;
        for (uint8_t i = 0U; i < 8U; i++) {
            if (g_ui_ctx.commitment[i] != g_ble_ctx.commitment_digest[i]) {
                match = 0U; break;
            }
        }
        status = match ? 1U : 2U;
        g_ui_ctx.commitment_verified = (match == 1U);
    }

    if (status == 0U) {
        /* Waiting — animated spinner at left */
        const char *spin = "|/-\\";
        char sp[2U] = { spin[(f / 4U) % 4U], '\0' };
        hal_ui_text(4U, 38U, sp, HAL_UI_WHITE);
        hal_ui_text(14U, 38U, "Waiting for peer", HAL_UI_WHITE);

        /* Sweeping underline */
        uint8_t ul_x = (uint8_t)((f * 3U) % 128U);
        draw_hline(ul_x, 37U, 10U);

    } else if (status == 1U) {
        /* MATCH — expanding rings from centre of display */
        hal_ui_text(4U, 38U, "\x7e MATCH \x7e", HAL_UI_WHITE);

        uint8_t ring_r = (uint8_t)((f % 16U) * 2U);
        if (ring_r > 0U && ring_r < 30U) {
            draw_circle(64, 44, ring_r);
        }
        uint8_t ring2_r = (uint8_t)(((f + 8U) % 16U) * 2U);
        if (ring2_r > 0U && ring2_r < 30U) {
            draw_circle(64, 44, ring2_r);
        }

        /* Confirmation sub-panel */
        uint32_t now_ms   = (uint32_t)(esp_timer_get_time() / 1000LL);
        uint32_t elapsed  = (g_ui_ctx.crypto_confirm_start_ms != 0U &&
                             now_ms >= g_ui_ctx.crypto_confirm_start_ms)
                          ? (now_ms - g_ui_ctx.crypto_confirm_start_ms) : 0U;
        uint32_t rem_ms   = (elapsed < 30000U) ? (30000U - elapsed) : 0U;
        uint8_t  cd_secs  = (uint8_t)(rem_ms / 1000U);

        char cd_str[13U];
        (void)snprintf(cd_str, sizeof(cd_str), "Confirm: %us", (unsigned)cd_secs);
        hal_ui_text(24U, 54U, cd_str, HAL_UI_WHITE);

        if (g_ui_ctx.button_pressed) {
            (void)ui_manager_transition_to(UI_STATE_CHAT);
            g_ui_ctx.transition_ready = true;
        }
        if (rem_ms == 0U) {
            (void)ui_manager_transition_to(UI_STATE_CHAT);
            g_ui_ctx.transition_ready = true;
        }

    } else {
        /* MISMATCH — pulsing X with bracket animation */
        uint8_t blink = (uint8_t)((f / 5U) % 2U);
        if (blink == 0U) {
            hal_ui_text(4U,  38U, ">", HAL_UI_WHITE);
            hal_ui_text(14U, 38U, "MISMATCH", HAL_UI_WHITE);
            hal_ui_text(62U, 38U, "<", HAL_UI_WHITE);
        }
        hal_ui_text(4U, 50U, "Restart to re-pair", HAL_UI_WHITE);
    }

    g_ui_ctx.anim.frame_count++;
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
            }
        } else if (g_ui_ctx.current_state == UI_STATE_COUNTDOWN) {
            /* Mark countdown start time */
            g_ui_ctx.countdown_start_ms = now_ms;
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
            memset(g_ui_ctx.commitment, 0U, 8U);
            memset(g_ui_ctx.peer_commitment, 0U, 8U);
            g_ui_ctx.commitment_verified = false;
            g_ui_ctx.crypto_confirm_start_ms = now_ms;
        } else if (g_ui_ctx.current_state == UI_STATE_NONCE_EXHAUSTED) {
            g_ui_ctx.error_start_ms = now_ms;
        } else if (g_ui_ctx.current_state == UI_STATE_ERROR) {
            g_ui_ctx.reject_sequence_start_ms = now_ms;
            g_ui_ctx.error_start_ms = now_ms;
        }
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
                /* Confirm code and start countdown */
                (void)ui_manager_transition_to(UI_STATE_COUNTDOWN);
                g_ui_ctx.countdown_start_ms = now_ms;
                g_ui_ctx.transition_ready = true;
            } else {
                /* Short press: move to next digit */
                if (g_ui_ctx.code_selected < 3U) { g_ui_ctx.code_selected++; }
                else { /* wrap to first digit */ g_ui_ctx.code_selected = 0U; }
            }
        }
    } else if (g_ui_ctx.current_state == UI_STATE_DISCOVERY) {
       /* Button press during discovery: if peer found, connect and transition */
       if (g_ui_ctx.button_pressed && !g_ui_ctx.button_prev) {
           if (g_ble_ctx.discovered) {
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
    } else if (g_ui_ctx.current_state == UI_STATE_ERROR ||
               g_ui_ctx.current_state == UI_STATE_NONCE_EXHAUSTED) {
        uint32_t start_ms = (g_ui_ctx.current_state == UI_STATE_ERROR)
                            ? g_ui_ctx.reject_sequence_start_ms
                            : g_ui_ctx.error_start_ms;
        if (start_ms != 0U) {
            uint32_t elapsed_ms = (now_ms >= start_ms) ? (now_ms - start_ms) : 0U;
            uint32_t seq_ms = (uint32_t)(CEEPEW_RGB_REJECT_SEQUENCE_CT * 2U * CEEPEW_RGB_ERROR_BLINK_MS);
            if (elapsed_ms >= seq_ms) {
                session_wipe();
            }
        }
    }

    uint32_t now_s = now_ms / 1000U;
    if (!g_ble_ctx.discovered &&
        (g_ble_ctx.state == BLE_SCANNING ||
         g_ble_ctx.state == BLE_ADVERTISING ||
         g_ble_ctx.state == BLE_ADVERTISING_AND_SCANNING) &&
        g_ble_ctx.discovery_start_ts != 0U &&
        now_s >= g_ble_ctx.discovery_start_ts &&
        (now_s - g_ble_ctx.discovery_start_ts) >= CEEPEW_PAIRING_TIMEOUT_S) {
        session_wipe();
    }

    if (g_ble_ctx.state == BLE_PAIRING &&
        g_ble_ctx.pairing_start_ts != 0U &&
        now_s >= g_ble_ctx.pairing_start_ts &&
        (now_s - g_ble_ctx.pairing_start_ts) >= CEEPEW_PAIRING_TIMEOUT_S) {
        session_wipe();
    }

    /* Remember previous button state for edge detection */
    g_ui_ctx.button_prev = g_ui_ctx.button_pressed;

    return CEEPEW_OK;
}

CeePewErr_t ui_manager_draw(void)
{
    CEEPEW_ASSERT(s_ui_manager_initialised, CEEPEW_ERR_PARAM);

    /* Dispatch to current screen renderer */
    CeePewErr_t err = CEEPEW_OK;

    switch (g_ui_ctx.current_state) {
        case UI_STATE_BOOT:
            err = render_boot_anim();
            break;
        case UI_STATE_DISCOVERY:
            err = render_discovery();
            break;
        case UI_STATE_CODE_ENTRY:
            err = render_code_entry();
            break;
        case UI_STATE_COUNTDOWN:
            err = render_countdown();
            break;
        case UI_STATE_CONFIRM:
            err = render_confirm();
            break;
        case UI_STATE_KEYDER:
            err = render_keyder_anim();
            break;
        case UI_STATE_FINGERPRINT:
            err = render_fingerprint();
            break;
        case UI_STATE_FINGERPRINT_CONFIRM:
            err = render_fingerprint_confirm();
            break;
        case UI_STATE_CHAT:
            err = render_chat();
            break;
        case UI_STATE_CRYPTOGRAM:
            err = render_cryptogram();
            break;
        case UI_STATE_NONCE_EXHAUSTED:
            err = ui_show_nonce_exhausted();
            break;
        case UI_STATE_CODE_INCORRECT:
            err = render_code_incorrect();
            break;
        case UI_STATE_CODE_DIFFERENT:
            err = render_code_different();
            break;
        case UI_STATE_ERROR:
            err = render_error();
            break;
        default:
            return CEEPEW_ERR_PARAM;
    }

    if (err != CEEPEW_OK) { return err; }

    /* Flush framebuffer to display */
    return hal_ui_flush();
}

CeePewErr_t ui_manager_transition_to(UIState_t next_state)
{
    CEEPEW_ASSERT(next_state <= UI_STATE_ERROR, CEEPEW_ERR_PARAM);
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

    /* Fingerprint hex (16 bytes = 32 hex chars, wrap at 8 chars per line) */
    char fp_hex[65];
    for (uint8_t i = 0U; i < 16U; i++) {
        (void)snprintf(&fp_hex[i * 2U], 3U, "%02X", fingerprint[i]);
    }
    fp_hex[32] = '\0';

    /* Line 1: First 8 bytes of fingerprint */
    fp_hex[16] = '\0';
    hal_ui_text(0U, 14U, fp_hex, HAL_UI_WHITE);

    /* Line 2: Last 8 bytes of fingerprint */
    hal_ui_text(0U, 24U, &fp_hex[16], HAL_UI_WHITE);

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

    char fp_hex[33];
    for (uint8_t i = 0U; i < 16U; i++) {
        (void)snprintf(&fp_hex[i * 2U], 3U, "%02X", fingerprint[i]);
    }
    fp_hex[32] = '\0';

    char row1[17];
    memcpy(row1, fp_hex, 16U);
    row1[16] = '\0';
    hal_ui_text(0U, 16U, row1, HAL_UI_WHITE);

    char row2[17];
    memcpy(row2, fp_hex + 16U, 16U);
    row2[16] = '\0';
    hal_ui_text(0U, 26U, row2, HAL_UI_WHITE);

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
    hal_ui_text(16U, 8U, "SECURITY RESET", HAL_UI_WHITE);
    hal_ui_text(8U, 22U, "Fingerprint rejected", HAL_UI_WHITE);
    hal_ui_text(8U, 34U, "Wiping session...", HAL_UI_WHITE);

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    uint32_t elapsed_ms = (now_ms >= g_ui_ctx.reject_sequence_start_ms)
                          ? (now_ms - g_ui_ctx.reject_sequence_start_ms)
                          : 0U;
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

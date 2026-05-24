/* components/ceepew_hal/ui_manager.c
 *
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
#include <string.h>
#include <stdio.h>

/* Design note: The UI manager is a simple state machine that tracks the
   current screen, animation state, and user input. Each screen state calls
   its corresponding render function. Transitions between screens are driven
   by completion flags and user input. This architecture allows all 9 screen
   types (Sprints 8-12) to coexist in one efficient state machine. */

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

CeePewErr_t ui_manager_init(void)
{
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

    s_ui_manager_initialised = true;
    return CEEPEW_OK;
}

/* Lock shield icon pixel art (8 wide × 16 tall, LSB = leftmost pixel)
   Design: Stylized shield with lock cutout */
/* Note: This icon is not used in the current render functions but is kept
   for potential future use in Sprint 8+ UI updates. */

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
static void draw_radial_segment(int16_t cx, int16_t cy,
                                int16_t ex, int16_t ey,
                                uint8_t r_near, uint8_t r_far)
{
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
static uint8_t rssi_to_bars(int8_t rssi)
{
    if (rssi >= -50) { return 5U; }
    if (rssi >= -60) { return 4U; }
    if (rssi >= -70) { return 3U; }
    if (rssi >= -80) { return 2U; }
    return 1U;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 2 — DATA TABLES & CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Radar display constants for discovery visualization.
   RDR_CX, RDR_CY = center of 64×64 radar area on screen
   RDR_R1, RDR_R2, RDR_R3 = concentric ring radii */
#define RDR_CX 31
#define RDR_CY 31
#define RDR_R1 8U
#define RDR_R2 16U
#define RDR_R3 24U

/* 16-step radar sweep endpoints, r=24 from centre (31,31).
 * Angles: 0°=E, stepping 22.5° clockwise.
 * Computed: ex = 31 + round(24*cos(θ)), ey = 31 + round(24*sin(θ))  */
typedef struct { int16_t ex; int16_t ey; } SweepPt_t;

static const SweepPt_t SWEEP16[16U] = {
    { 55, 31 }, /* 0  E    */
    { 53, 40 }, /* 1  ESE  */
    { 48, 48 }, /* 2  SE   */
    { 40, 53 }, /* 3  SSE  */
    { 31, 55 }, /* 4  S    */
    { 22, 53 }, /* 5  SSW  */
    { 14, 48 }, /* 6  SW   */
    {  9, 40 }, /* 7  WSW  */
    {  7, 31 }, /* 8  W    */
    {  9, 22 }, /* 9  WNW  */
    { 14, 14 }, /* 10 NW   */
    { 22,  9 }, /* 11 NNW  */
    { 31,  7 }, /* 12 N    */
    { 40,  9 }, /* 13 NNE  */
    { 48, 14 }, /* 14 NE   */
    { 53, 22 }, /* 15 ENE  */
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
static void draw_progress_bar(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t pct)
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
static void draw_scroll_text(uint8_t x, uint8_t y,
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
       const uint8_t FINAL_Y      = 14U;

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

       /* Percentage counter */
       uint8_t pct = (seg_count * 100U) / 12U;
       char pct_str[5U];
       (void)snprintf(pct_str, sizeof(pct_str), "%3u%%", (unsigned int)pct);
       hal_ui_text(104U, 44U, pct_str, HAL_UI_WHITE);
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

       /* Subtitle fades in */
       if (bf >= 10U) {
           hal_ui_text(16U, 30U, "SECURE MESSENGER", HAL_UI_WHITE);
       }
    }

    /* ── Phase 5 (f 110–129): READY pulses, tick-mark flash ── */
    if (f >= 110U) {
       hal_ui_text(16U, 30U, "SECURE MESSENGER", HAL_UI_WHITE);

       /* Full border now solid */
       draw_hline(0U, 0U, 128U);
       draw_hline(0U, 63U, 128U);
       draw_vline(0U, 0U, 64U);
       draw_vline(127U, 0U, 64U);

       /* READY text blinks at ~3Hz (5 frames on, 5 off) */
       uint8_t blink = (uint8_t)((f - 110U) % 10U);
       if (blink < 5U) {
           hal_ui_text(50U, 14U, "READY", HAL_UI_WHITE);
       }

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

#define RDR_CX   31     /* radar centre x */
#define RDR_CY   31     /* radar centre y */
#define RDR_R1    8U    /* inner ring radius  */
#define RDR_R2   16U    /* middle ring radius */
#define RDR_R3   24U    /* outer ring radius  */
#define INFO_X   65U    /* right panel start  */

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

    /* Panel divider */
    draw_vline(63U, 0U, 64U);
}

static void draw_sweep_arm(uint8_t pos)
{
    /* Trail 3 (oldest, faintest): arm to 45% of outer radius */
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

    /* Active arm: full bright line */
    draw_line(RDR_CX, RDR_CY, SWEEP16[pos].ex, SWEEP16[pos].ey);

    /* Bright 3×3 cap at arm tip */
    HalUIRect_t cap = {
        .x = (uint8_t)((uint8_t)SWEEP16[pos].ex - 1U),
        .y = (uint8_t)((uint8_t)SWEEP16[pos].ey - 1U),
        .w = 3U, .h = 3U
    };
    hal_ui_rect_fill(&cap, HAL_UI_WHITE);
}

/* Pulsing ring that expands from (bx,by) when peer found.
 * ring_r grows each frame, fades when > RDR_R3. */
static void draw_peer_blip(uint8_t bx, uint8_t by, uint32_t f)
{
    /* Solid blip — blinks at ~6Hz */
    uint8_t blip_on = (uint8_t)((f / 5U) % 2U);
    if (blip_on == 0U) {
        HalUIRect_t blip = { .x = bx - 2U, .y = by - 2U, .w = 5U, .h = 5U };
        hal_ui_rect_fill(&blip, HAL_UI_WHITE);
    }

    /* Outward expanding ring every 20 frames */
    uint8_t ring_r = (uint8_t)((f % 20U));
    if (ring_r > 0U && ring_r <= 12U) {
        draw_circle((int16_t)bx, (int16_t)by, ring_r);
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

    /* ── FIX B(1): ALWAYS clear before drawing — prevents artifact bleed ── */
    hal_ui_clear();

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);

    /* ── Row 0–7: Status bar ── */
    hal_ui_text(0U, 0U, "P1", HAL_UI_WHITE);
    hal_ui_text(92U, 0U, "DISCO", HAL_UI_WHITE);

    /* ── Rows 8–47: FULL-FEATURED RADAR WITH ADVANCED HELPERS ─────────────── */
    /*
     * Layout: 128-pixel width split into two 64-pixel panels
     * Left panel (x=0–63):   Radar visualization (RDR_CX, RDR_CY = 31,31 local)
     * Right panel (x=64–127): Status & peer list
     *
     * Left radar:
     *   - Static structure: 3 concentric rings (r=8,16,24)
     *   - Crosshairs centered at (31, 31)
     *   - Rotating sweep arm with trail effects
     *   - Peer blips when discovered
     */

    /* ── LEFT PANEL: Radar visualization ── */
    /* Draw static radar structure (rings + crosshairs + centre dot) */
    draw_radar_static();

    /* Animated sweep arm: 16 positions, ~150ms per step → full rotation every 2.4s */
    uint8_t sweep_pos = (uint8_t)((now_ms / 150U) % 16U);
    draw_sweep_arm(sweep_pos);

    /* If a real peer was discovered via BLE, show its blip and details. Otherwise
     * fall back to the simulated placeholder used during early UI testing. */
    if (g_ble_ctx.discovered) {
        uint32_t now_ms_local = (uint32_t)(esp_timer_get_time() / 1000LL);

        /* Smoothed RSSI back to dBm (stored ×8) */
        int16_t rssi_smooth = (int16_t)(g_ble_ctx.peer_rssi_smooth_x8 / 8);
        if (rssi_smooth < -90) { rssi_smooth = -90; }
        if (rssi_smooth > -30) { rssi_smooth = -30; }

        /* Map smoothed RSSI to radial distance: -30dBm -> 2, -90dBm -> RDR_R3-1 */
        uint8_t blip_r = (uint8_t)((((int16_t)(-rssi_smooth - 30) * (int16_t)(RDR_R3 - 3U)) / 60) + 2U);

        /* Angle: base derived from MAC low byte (consistent), plus small wobble from instant noise */
        uint8_t base_idx = (uint8_t)(g_ble_ctx.peer_mac[5] % 16U);
        int16_t rssi_delta = (int16_t)((int16_t)g_ble_ctx.peer_rssi - rssi_smooth);
        int8_t wobble = (int8_t)((rssi_delta > 3) ? 2 : (rssi_delta < -3) ? -2 : 0);
        uint8_t blip_idx = (uint8_t)((int16_t)base_idx + wobble + 16U) % 16U;

        int16_t ex = SWEEP16[blip_idx].ex;
        int16_t ey = SWEEP16[blip_idx].ey;
        int16_t dx = ex - RDR_CX;
        int16_t dy = ey - RDR_CY;

        int16_t bx = (int16_t)RDR_CX + (int16_t)(((int32_t)dx * blip_r) / (int32_t)RDR_R3);
        int16_t by = (int16_t)RDR_CY + (int16_t)(((int32_t)dy * blip_r) / (int32_t)RDR_R3);

        /* Staleness: blink faster when older than 2s */
        uint32_t age_ms = (g_ble_ctx.last_seen_ms == 0U) ? 0U : (now_ms_local - g_ble_ctx.last_seen_ms);
        uint32_t blink_rate = (age_ms > 2000U) ? 100U : 180U; /* ms per half-period */
        bool blink_on = ((now_ms_local / blink_rate) & 1U) != 0U;

        if (blink_on && bx >= 0 && bx < 128 && by >= 10 && by < 64) {
            int8_t blip_size = (age_ms < 1000U) ? 1 : 0; /* 3x3 if fresh, 1x1 if stale */
            for (int8_t ox = -blip_size; ox <= blip_size; ox++) {
                for (int8_t oy = -blip_size; oy <= blip_size; oy++) {
                    int16_t fx = bx + ox;
                    int16_t fy = by + oy;
                    if (fx >= 0 && fx < 128 && fy >= 10 && fy < 64) {
                        draw_pixel((uint8_t)fx, (uint8_t)fy);
                    }
                }
            }
        }

        /* Status: smoothed RSSI + hit count + age */
        char line1[22]; char line2[22];
        (void)snprintf(line1, sizeof(line1), "PEER %02X%02X  ~%ddBm",
                       g_ble_ctx.peer_mac[4], g_ble_ctx.peer_mac[5], (int)rssi_smooth);
        (void)snprintf(line2, sizeof(line2), "hits:%u age:%lums",
                       (unsigned)g_ble_ctx.scan_hit_count, (unsigned long)age_ms);
        hal_ui_text(0U, 48U, line1, HAL_UI_WHITE);
        hal_ui_text(64U, 48U, line2, HAL_UI_WHITE);
    } else {
        /* Preserve a simple simulation for when no real peer exists */
        if ((now_ms / 3000U) % 2U == 0U) {
            uint8_t peer_bx = (uint8_t)(RDR_CX - 12U);
            uint8_t peer_by = (uint8_t)(RDR_CY + 10U);
            draw_peer_blip(peer_bx, peer_by, now_ms);
        }

        /* If no real peer, clear status area */
        hal_ui_text(68U, 16U, "--- scanning ---", HAL_UI_WHITE);
    }

    /* ── RIGHT PANEL: Status & peer information (x=64–127) ── */
    hal_ui_text(68U, 8U, "PEERS", HAL_UI_WHITE);

    if (g_ble_ctx.discovered) {
        char macbuf[18U];
        (void)snprintf(macbuf, sizeof(macbuf), "%02X:%02X:%02X:%02X:%02X:%02X",
                       g_ble_ctx.peer_mac[0], g_ble_ctx.peer_mac[1], g_ble_ctx.peer_mac[2],
                       g_ble_ctx.peer_mac[3], g_ble_ctx.peer_mac[4], g_ble_ctx.peer_mac[5]);
        hal_ui_text(68U, 16U, macbuf, HAL_UI_WHITE);
        uint8_t bars = rssi_to_bars((int8_t)(g_ble_ctx.peer_rssi_smooth_x8 / 8));
        draw_rssi_bars(68U, 24U, bars);
    } else {
        hal_ui_text(68U, 16U, "--- scanning ---", HAL_UI_WHITE);
    }

    /* Row 32: Instruction text */
    hal_ui_text(68U, 32U, "Press to pair", HAL_UI_WHITE);

    /* Row 40: BLE state indicator */
    const char *ble_state = "BLE: IDLE";
    switch (transport_ble_get_state()) {
        case BLE_IDLE:        ble_state = "BLE: IDLE";    break;
        case BLE_ADVERTISING: ble_state = "BLE: ADV";     break;
        case BLE_SCANNING:    ble_state = "BLE: SCAN";    break;
        case BLE_CONNECTED:   ble_state = "BLE: CONN";    break;
        case BLE_PAIRING:     ble_state = "BLE: PAIR";    break;
        case BLE_DONE:        ble_state = "BLE: DONE";    break;
        default:              ble_state = "BLE: ?";       break;
    }
    hal_ui_text(68U, 40U, ble_state, HAL_UI_WHITE);

    /* ── Rows 48–55: Bottom status bar ─────────────────────────────────────── */
    /*
     * FIX B(2): Status text is drawn unconditionally.
     * "SCANNING" with animated trailing dots cycles at 500ms intervals.
     */
    static const char *const SCAN_STATES[4] = {
        "SCANNING   ",
        "SCANNING.  ",
        "SCANNING.. ",
        "SCANNING..."
    };
    uint8_t dot_idx = (uint8_t)((now_ms / 500U) % 4U);
    hal_ui_text(0U, 48U, SCAN_STATES[dot_idx], HAL_UI_WHITE);

    /* Signal strength hint (simulation) */
    if ((now_ms / 3000U) % 2U == 0U) {
        uint8_t rssi_est = (int8_t)(-60 + (now_ms / 100U) % 30);  /* -60 to -30 dBm */
        char rssi_str[12];
        (void)snprintf(rssi_str, sizeof(rssi_str), "RSSI:%d", rssi_est);
        hal_ui_text(64U, 48U, rssi_str, HAL_UI_WHITE);
    } else {
        hal_ui_text(64U, 48U, "No signal ", HAL_UI_WHITE);
    }

    /* ── Rows 56–63: Navigation bar ── */
    hal_ui_text(52U, 57U, "[P1]", HAL_UI_WHITE);

    return CEEPEW_OK;
}

static CeePewErr_t render_code_entry(void)
{
    hal_ui_clear();

    uint32_t f = g_ui_ctx.anim.frame_count;

    /* Title */
    hal_ui_text(22U, 2U, "ENTER PAIRING CODE", HAL_UI_WHITE);
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

        /* Digit character (large, centred in cell) */
        char digit_str[2U] = { (char)('0' + g_ui_ctx.code_digits[i]), '\0' };

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
    hal_ui_text(4U, 56U, "Turn=digit  Hold=confirm", HAL_UI_WHITE);

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
    hal_ui_text(30U, 2U, "CODE CONFIRMED", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    /* Large countdown number — centred */
    char sec_str[4U];
    (void)snprintf(sec_str, sizeof(sec_str), "%2u", (unsigned)secs);
    hal_ui_text(56U, 18U, sec_str, HAL_UI_WHITE);
    hal_ui_text(18U, 18U, "Syncing in", HAL_UI_WHITE);
    hal_ui_text(72U, 18U, "sec", HAL_UI_WHITE);

    /* Animated progress arc — use a shrinking bar */
    uint32_t fill = (rem_ms * 110U) / total_ms;  /* 0..110 */
    if (fill > 110U) { fill = 110U; }

    /* Bar border */
    HalUIRect_t border = { .x = 9U, .y = 34U, .w = 110U, .h = 10U };
    hal_ui_rect(&border, HAL_UI_WHITE);

    if (fill > 0U) {
        /* Pulsing fill — brightest at right edge */
        HalUIRect_t bar = { .x = 11U, .y = 36U, .w = (uint8_t)fill, .h = 6U };
        hal_ui_rect_fill(&bar, HAL_UI_WHITE);

        /* Bright leading edge 2px wide */
        uint8_t edge_x = (uint8_t)(11U + fill);
        if (edge_x < 120U) {
            draw_vline(edge_x, 35U, 8U);
        }
    }

    /* Pulsing ring around countdown digit */
    uint8_t ring_r = (uint8_t)((f % 20U));
    if (ring_r > 0U && ring_r < 12U) {
        draw_circle(64, 21, ring_r);
    }

    /* "Waiting for peer" blink */
    uint8_t blink = (uint8_t)((f / 10U) % 2U);
    if (blink == 0U) {
        hal_ui_text(12U, 48U, "Waiting for peer...", HAL_UI_WHITE);
    }

    /* Tick marks at bar edges */
    draw_vline(9U,  30U, 4U);
    draw_vline(119U,30U, 4U);

    g_ui_ctx.anim.frame_count++;
    return CEEPEW_OK;
}

static CeePewErr_t render_confirm(void)
{
    hal_ui_clear();

    uint32_t f = g_ui_ctx.anim.frame_count;

    hal_ui_text(22U, 2U, "CONFIRM PAIRING?", HAL_UI_WHITE);
    draw_hline(0U, 12U, 128U);

    /* Animated concentric rings — draw at centre, expanding slowly */
    uint8_t ring_phase = (uint8_t)((f / 3U) % 24U);
    if (ring_phase > 0U && ring_phase <= 8U) {
        draw_circle(64, 32, ring_phase * 2U);
    }

    /* Lock icon — simplified: a rect (body) + arc (shackle) */
    draw_circle(64, 28, 6);                       /* shackle arc (top half) */
    HalUIRect_t lock_body = { .x = 56U, .y = 30U, .w = 16U, .h = 12U };
    hal_ui_rect_fill(&lock_body, HAL_UI_WHITE);
    /* Keyhole would go at (62,33) but we can't clear pixels, so skip */

    /* Prompt box at bottom — pulses */
    uint8_t blink = (uint8_t)((f / 8U) % 2U);
    HalUIRect_t prompt = { .x = 14U, .y = 48U, .w = 100U, .h = 14U };
    if (blink == 0U) {
        hal_ui_rect(&prompt, HAL_UI_WHITE);
        hal_ui_text(24U, 52U, "HOLD BUTTON TO PAIR", HAL_UI_WHITE);
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

    /* After full reveal: "Press to confirm" blinks */
    if (reveal >= 32U) {
        uint8_t blink = (uint8_t)((f / 8U) % 2U);
        if (blink == 0U) {
            hal_ui_text(8U, 52U, ">>> Press to confirm <<<", HAL_UI_WHITE);
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

    hal_ui_text(22U, 2U, "VERIFY IDENTITY", HAL_UI_WHITE);
    draw_hline(0U, 11U, 128U);

    /* Full fingerprint hex */
    char fp_hex[33U];
    for (uint8_t i = 0U; i < 16U; i++) {
        uint8_t  b = g_ui_ctx.fingerprint[i];
        uint8_t  h = (b >> 4U) & 0x0FU;
        uint8_t  l = b & 0x0FU;
        fp_hex[i * 2U]      = (h < 10U) ? (char)('0' + h) : (char)('A' + h - 10U);
        fp_hex[i * 2U + 1U] = (l < 10U) ? (char)('0' + l) : (char)('A' + l - 10U);
    }
    fp_hex[32U] = '\0';

    char row1[17U]; (void)memcpy(row1, fp_hex, 16U); row1[16U] = '\0';
    char row2[17U]; (void)memcpy(row2, fp_hex + 16U, 16U); row2[16U] = '\0';
    hal_ui_text(4U, 14U, row1, HAL_UI_WHITE);
    hal_ui_text(4U, 24U, row2, HAL_UI_WHITE);

    /* Peer MAC */
    char mac_str[16U];
    (void)snprintf(mac_str, sizeof(mac_str), "Peer: %02X%02X",
                   g_ui_ctx.peer_mac[4U], g_ui_ctx.peer_mac[5U]);
    hal_ui_text(4U, 36U, mac_str, HAL_UI_WHITE);

    /* Accept / reject buttons — animated outlines */
    uint8_t blink = (uint8_t)((f / 6U) % 2U);

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
        hal_ui_text(8U, y_pos, "Waiting for peer...", HAL_UI_WHITE);
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
            for (uint8_t i = 0U; i < 4U; i++) { g_ui_ctx.code_digits[i] = 0U; }
            g_ui_ctx.code_selected = 0U;
            g_ui_ctx.button_prev = false;
            g_ui_ctx.button_press_start_ms = 0U;
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
        /* Map pot value (0-255) to digit 0-9 */
        uint8_t pot = g_ui_ctx.user_input;
        uint8_t digit = (uint8_t)((uint16_t)pot * 10U / 256U);
        if (g_ui_ctx.code_selected < 4U) {
            g_ui_ctx.code_digits[g_ui_ctx.code_selected] = digit;
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
            if (dur >= HOLD_MS) {
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
}

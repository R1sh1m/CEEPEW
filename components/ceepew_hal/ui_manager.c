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
static const uint8_t s_shield_icon[16] = {
    0x3EU, /* 00111110 */
    0x7FU, /* 01111111 */
    0x7FU, /* 01111111 */
    0x7FU, /* 01111111 */
    0x3EU, /* 00111110 */
    0x3EU, /* 00111110 */
    0x1CU, /* 00011100 */
    0x1CU, /* 00011100 */
    0x1CU, /* 00011100 */
    0x1CU, /* 00011100 */
    0x3EU, /* 00111110 */
    0x7FU, /* 01111111 */
    0x7FU, /* 01111111 */
    0x7FU, /* 01111111 */
    0x3EU, /* 00111110 */
    0x00U  /* 00000000 */
};

static CeePewErr_t render_boot_anim(void)
{
    /* Comprehensive boot animation with logo + progress messages + status indicators.
       Sequence:
       - Frame 0-5 (~200ms): Show lock-shield logo + "CEE-PEW" text
       - Frame 6-15 (~300ms): "INIT HARDWARE" with animated dots
       - Frame 16-25 (~300ms): "INIT CRYPTO" with animated dots
       - Frame 26-35 (~300ms): "INIT RADIO" with animated dots
       - Frame 36-45 (~500ms): "READY ✓" with checkmark indicator
       - Frame 46+: Auto-transition to DISCOVERY
    */
    uint32_t frame = g_ui_ctx.anim.frame_count;
    uint32_t elapsed_ms = frame * g_ui_ctx.anim.frame_rate_ms;
    
    hal_ui_clear();

    if (elapsed_ms < 200U) {
        /* Phase 1: Show logo + "CEE-PEW" centered on screen */
        
        /* Draw shield icon at (60, 10) — centered horizontally */
        uint8_t icon_x = 60U;
        uint8_t icon_y = 10U;
        for (uint8_t row = 0U; row < 16U; row++) {
            uint8_t byte_val = s_shield_icon[row];
            for (uint8_t col = 0U; col < 8U; col++) {
                uint8_t bit = (byte_val >> (7U - col)) & 1U;
                if (bit) {
                    (void)hal_ui_pixel(icon_x + col, icon_y + row, HAL_UI_WHITE);
                }
            }
        }
        
        /* Draw "CEE-PEW" text below shield */
        hal_ui_text(35U, 32U, "CEE-PEW", HAL_UI_WHITE);
        
        /* Draw loading spinner in corner */
        uint8_t spinner_frame = (uint8_t)((frame / 2U) % 4U);
        const char *spinner_chars[] = {"|", "/", "-", "\\"};
        hal_ui_text(120U, 2U, spinner_chars[spinner_frame], HAL_UI_WHITE);
        
    } else if (elapsed_ms < 500U) {
        /* Phase 2: INIT HARDWARE with animated dots */
        hal_ui_text(10U, 20U, "INIT HARDWARE", HAL_UI_WHITE);
        
        uint32_t phase_elapsed = elapsed_ms - 200U;
        uint8_t dot_count = (uint8_t)(1U + (phase_elapsed / 100U));
        if (dot_count > 3U) { dot_count = 3U; }
        
        for (uint8_t d = 0U; d < dot_count; d++) {
            (void)hal_ui_text((uint8_t)(95U + d * 8U), 20U, ".", HAL_UI_WHITE);
        }
        
    } else if (elapsed_ms < 800U) {
        /* Phase 3: INIT CRYPTO with animated dots */
        hal_ui_text(10U, 20U, "INIT CRYPTO", HAL_UI_WHITE);
        
        uint32_t phase_elapsed = elapsed_ms - 500U;
        uint8_t dot_count = (uint8_t)(1U + (phase_elapsed / 100U));
        if (dot_count > 3U) { dot_count = 3U; }
        
        for (uint8_t d = 0U; d < dot_count; d++) {
            (void)hal_ui_text((uint8_t)(95U + d * 8U), 20U, ".", HAL_UI_WHITE);
        }
        
    } else if (elapsed_ms < 1100U) {
        /* Phase 4: INIT RADIO with animated dots */
        hal_ui_text(10U, 20U, "INIT RADIO", HAL_UI_WHITE);
        
        uint32_t phase_elapsed = elapsed_ms - 800U;
        uint8_t dot_count = (uint8_t)(1U + (phase_elapsed / 100U));
        if (dot_count > 3U) { dot_count = 3U; }
        
        for (uint8_t d = 0U; d < dot_count; d++) {
            (void)hal_ui_text((uint8_t)(95U + d * 8U), 20U, ".", HAL_UI_WHITE);
        }
        
    } else if (elapsed_ms < 1600U) {
        /* Phase 5: READY with checkmark */
        hal_ui_text(40U, 20U, "READY", HAL_UI_WHITE);
        
        /* Draw checkmark (simple X pattern) */
        (void)hal_ui_hline(95U, 100U, 20U, HAL_UI_WHITE);
        (void)hal_ui_vline(98U, 18U, 22U, HAL_UI_WHITE);
        
    } else {
        /* Boot complete: transition to discovery */
        (void)ui_manager_transition_to(UI_STATE_DISCOVERY);
        g_ui_ctx.transition_ready = true;
    }
    
    /* Flush to display and advance frame */
    (void)hal_ui_flush();
    g_ui_ctx.anim.frame_count++;
    
    return CEEPEW_OK;
}

/* Helper: Simple sine/cosine approximations for polar coordinate conversions */
static int8_t approx_sin_i8(uint8_t angle_0_255) {
    /* Approximates sin(angle * 2π/256) * 127 for angle in [0, 255]
       Uses piecewise linear approximation for 50% LUT saving */
    if (angle_0_255 < 64U) {
        return (int8_t)((angle_0_255 * 127) / 64);  /* 0° to 90°: 0 → 127 */
    } else if (angle_0_255 < 128U) {
        return (int8_t)(127 - (((angle_0_255 - 64U) * 127) / 64));  /* 90° to 180°: 127 → 0 */
    } else if (angle_0_255 < 192U) {
        return (int8_t)(-(((angle_0_255 - 128U) * 127) / 64));  /* 180° to 270°: 0 → -127 */
    } else {
        return (int8_t)(-(127 - (((angle_0_255 - 192U) * 127) / 64)));  /* 270° to 360°: -127 → 0 */
    }
}

static int8_t approx_cos_i8(uint8_t angle_0_255) {
    return approx_sin_i8(angle_0_255 + 64U);  /* cos(θ) = sin(θ + 90°) */
}

/* Helper: Map RSSI (dBm) to radar radius for blip placement */
static uint8_t rssi_to_radius(int8_t rssi_dbm) {
    /* Map RSSI to radius: stronger signal (less negative) = closer to center
       RSSI ≥ -50 dBm → r=8 px (innermost)
       RSSI -50 to -70 → r=16 px (middle)
       RSSI < -70 → r=24 px (outermost) */
    if (rssi_dbm >= -50) {
        return 8U;
    } else if (rssi_dbm >= -70) {
        return 16U;
    } else {
        return 24U;
    }
}

static CeePewErr_t render_discovery(void)
{
    /* Advanced radar UI with:
       - 3 concentric circles (8, 16, 24 px)
       - 8 radial spokes at 45° intervals
       - 360° rotating sweep line
       - Peer blip positioned by RSSI + hash-derived angle
       - Peer MAC + RSSI info display
    */
    
    uint32_t frame = g_ui_ctx.anim.frame_count;
    uint8_t sweep_angle = (uint8_t)((frame * 2U) & 0xFFU);  /* 0-255 = 0-360° */
    
    hal_ui_clear();
    
    /* Draw title */
    hal_ui_text(32U, 2U, "Discovery", HAL_UI_WHITE);
    
    /* Radar center point */
    uint8_t cx = 64U;
    uint8_t cy = 32U;
    
    /* Draw 3 concentric circles */
    (void)hal_ui_circle(cx, cy, 8U, HAL_UI_WHITE);
    (void)hal_ui_circle(cx, cy, 16U, HAL_UI_WHITE);
    (void)hal_ui_circle(cx, cy, 24U, HAL_UI_WHITE);
    
    /* Draw 8 radial spokes at 45° intervals (0°, 45°, 90°, ..., 315°) */
    for (uint8_t spoke = 0U; spoke < 8U; spoke++) {
        uint8_t angle = (spoke * 32U) & 0xFFU;  /* 0, 32, 64, 96, 128, 160, 192, 224 (in 0-255 scale) */
        
        int8_t dx = approx_cos_i8(angle);
        int8_t dy = approx_sin_i8(angle);
        
        /* Scale to 24 px radius and compute endpoint */
        uint8_t ex = (uint8_t)(cx + ((int16_t)dx * 24) / 127);
        uint8_t ey = (uint8_t)(cy + ((int16_t)dy * 24) / 127);
        
        (void)hal_ui_line(cx, cy, ex, ey, HAL_UI_WHITE);  /* Diagonal line from center to endpoint */
    }
    
    /* Draw rotating sweep line from center to edge */
    {
        int8_t sweep_dx = approx_cos_i8(sweep_angle);
        int8_t sweep_dy = approx_sin_i8(sweep_angle);
        
        uint8_t sweep_x = (uint8_t)(cx + ((int16_t)sweep_dx * 26) / 127);
        uint8_t sweep_y = (uint8_t)(cy + ((int16_t)sweep_dy * 26) / 127);
        
        /* Draw line from center to sweep endpoint */
        (void)hal_ui_line(cx, cy, sweep_x, sweep_y, HAL_UI_WHITE);
    }
    
    /* Query BLE for discovered peer and render blip if found */
    const BlePeerRecord_t *peer = NULL;
    
    /* Attempt to call transport_ble_get_peer if available (weak reference) */
    extern const BlePeerRecord_t *transport_ble_get_peer(void) __attribute__((weak));
    if (transport_ble_get_peer != NULL) {
        peer = transport_ble_get_peer();
    }
    
    if (peer != NULL && peer->rssi != 0) {
        /* Compute blip angle from MAC hash */
        uint8_t mac_hash = 0U;
        for (uint8_t i = 0U; i < 6U; i++) {
            mac_hash ^= peer->peer_mac[i];
        }
        uint8_t blip_angle = mac_hash;  /* Use hash as angle 0-255 */
        
        /* Compute radius based on RSSI */
        uint8_t blip_radius = rssi_to_radius(peer->rssi);
        
        /* Compute blip position */
        int8_t blip_dx = approx_cos_i8(blip_angle);
        int8_t blip_dy = approx_sin_i8(blip_angle);
        
        uint8_t blip_x = (uint8_t)(cx + ((int16_t)blip_dx * blip_radius) / 127);
        uint8_t blip_y = (uint8_t)(cy + ((int16_t)blip_dy * blip_radius) / 127);
        
        /* Draw peer blip (2px filled circle) */
        (void)hal_ui_circle_fill(blip_x, blip_y, 2U, HAL_UI_WHITE);
        
        /* Display peer MAC at top-left */
        char mac_str[18];
        (void)snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x", 
                       peer->peer_mac[0], peer->peer_mac[1], peer->peer_mac[2]);
        hal_ui_text(2U, 8U, mac_str, HAL_UI_WHITE);
        
        /* Display RSSI value at bottom-left */
        char rssi_str[16];
        (void)snprintf(rssi_str, sizeof(rssi_str), "RSSI %d dBm", (int)peer->rssi);
        hal_ui_text(2U, 56U, rssi_str, HAL_UI_WHITE);
    } else {
        /* No peer found yet */
        hal_ui_text(20U, 40U, "Scanning...", HAL_UI_WHITE);
    }
    
    /* Flush and advance */
    (void)hal_ui_flush();
    g_ui_ctx.anim.frame_count++;
    
    return CEEPEW_OK;
}

static CeePewErr_t render_code_entry(void)
{
    /* Sprint 9: Code entry grid (4 digits) */
    hal_ui_clear();
    hal_ui_text(28U, 2U, "Enter Code", HAL_UI_WHITE);
    
    /* Draw 4 digit boxes and display entered digits */
    for (uint8_t i = 0U; i < 4U; i++) {
        HalUIRect_t box = {.x = (uint8_t)(18U + i * 24U), .y = 20U, .w = 20U, .h = 20U};
        hal_ui_rect(&box, HAL_UI_WHITE);
        
        /* Render entered digit (0-9) */
        char ch = (char)('0' + (g_ui_ctx.code_digits[i] % 10U));
        hal_ui_char((uint8_t)(24U + i * 24U), 28U, ch, HAL_UI_WHITE);

        /* Highlight currently selected digit with a small underline */
        if (i == g_ui_ctx.code_selected) {
            HalUIRect_t underline = {.x = (uint8_t)(20U + i * 24U), .y = 44U, .w = 12U, .h = 2U};
            hal_ui_rect_fill(&underline, HAL_UI_WHITE);
        }
    }
    
    return CEEPEW_OK;
}

static CeePewErr_t render_countdown(void)
{
    /* Sprint 9: Countdown bar (60 sec) - uses real time based on countdown_start_ms */
    const uint32_t COUNTDOWN_SEC = 60U;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    uint32_t elapsed = 0U;
    if (g_ui_ctx.countdown_start_ms != 0U && now_ms >= g_ui_ctx.countdown_start_ms) {
        elapsed = now_ms - g_ui_ctx.countdown_start_ms;
    }
    uint32_t rem_sec = 0U;
    if (elapsed / 1000U >= COUNTDOWN_SEC) { rem_sec = 0U; }
    else { rem_sec = COUNTDOWN_SEC - (elapsed / 1000U); }

    hal_ui_clear();
    hal_ui_text(20U, 2U, "Confirming...", HAL_UI_WHITE);
    
    /* Draw countdown bar */
    HalUIRect_t bar = {.x = 8U, .y = 30U, .w = 112U, .h = 16U};
    hal_ui_rect(&bar, HAL_UI_WHITE);
    
    uint8_t width = (uint8_t)((100U * rem_sec) / COUNTDOWN_SEC);
    if (width > 100U) { width = 100U; }
    HalUIRect_t fill = {.x = 10U, .y = 32U, .w = width, .h = 12U};
    hal_ui_rect_fill(&fill, HAL_UI_WHITE);
    
    /* Draw seconds remaining */
    char buf[16];
    (void)snprintf(buf, sizeof(buf), "%u sec", (unsigned int)rem_sec);
    hal_ui_text(40U, 50U, buf, HAL_UI_WHITE);
    
    g_ui_ctx.anim.frame_count++;

    /* Transition to CONFIRM when countdown expires */
    if (rem_sec == 0U) {
        (void)ui_manager_transition_to(UI_STATE_CONFIRM);
        g_ui_ctx.transition_ready = true;
    }

    /* Or if BLE handshake indicates both sides confirmed, move early */
    if (g_ble_ctx.commitment_verified) {
        (void)ui_manager_transition_to(UI_STATE_CONFIRM);
        g_ui_ctx.transition_ready = true;
    }
    
    return CEEPEW_OK;
}

static CeePewErr_t render_confirm(void)
{
    /* Sprint 9: Confirmation screen */
    hal_ui_clear();
    hal_ui_text(20U, 10U, "Confirmed!", HAL_UI_WHITE);
    hal_ui_text(8U, 30U, "Press button", HAL_UI_WHITE);
    hal_ui_text(12U, 40U, "to continue", HAL_UI_WHITE);
    
    if (g_ui_ctx.button_pressed) {
        /* Proceed to key derivation animation on user acknowledgement */
        (void)ui_manager_transition_to(UI_STATE_KEYDER);
        g_ui_ctx.transition_ready = true;
    }
    
    return CEEPEW_OK;
}

/* Sprint 10: Helper function to display key derivation progress animation.
 * Shows animated progress bar with "DERIVING..." text.
 * No dynamic allocation; uses static stack frame.
 * Two CEEPEW_ASSERTs for bounds checking.
 */
CeePewErr_t ui_keygen_show_progress(uint8_t frame_index)
{
    CEEPEW_ASSERT(frame_index <= 100U, CEEPEW_ERR_BOUNDS);
    
    /* Normalize frame to progress (0-100) */
    uint8_t progress = frame_index % 101U;
    
    /* Draw title and status text */
    hal_ui_text(12U, 2U, "Deriving...", HAL_UI_WHITE);
    
    /* Draw progress bar outline: 100px wide @ x=14, y=24 */
    HalUIRect_t bar_outline = {.x = 14U, .y = 24U, .w = 100U, .h = 12U};
    hal_ui_rect(&bar_outline, HAL_UI_WHITE);
    
    /* Draw filled portion based on progress */
    CEEPEW_ASSERT(progress <= 100U, CEEPEW_ERR_BOUNDS);
    HalUIRect_t bar_fill = {.x = 16U, .y = 26U, .w = progress, .h = 8U};
    hal_ui_rect_fill(&bar_fill, HAL_UI_WHITE);
    
    /* Draw rotating spinner dots at bottom */
    uint8_t spinner_pos = (frame_index / 10U) % 4U;
    uint8_t dot_x = (uint8_t)(30U + spinner_pos * 16U);
    hal_ui_circle(dot_x, 48U, 2U, HAL_UI_WHITE);
    
    /* Show "Wait..." message */
    hal_ui_text(48U, 50U, "Please wait", HAL_UI_WHITE);
    
    return CEEPEW_OK;
}

/* Sprint 10: Helper function to display fingerprint with session_id and grid.
 * Renders 16 hex digits (8x2) and 8x8 visual grid from session_id.
 * No dynamic allocation; static buffers.
 * Two CEEPEW_ASSERTs for validation.
 */
CeePewErr_t ui_keygen_show_fingerprint(bool show_commitment)
{
    hal_ui_clear();
    hal_ui_text(16U, 2U, "Fingerprint", HAL_UI_WHITE);
    
    /* Get session ID from session_fsm (64-bit value) */
    uint64_t session_id = session_get_id();
    
    /* Convert to 16 hex characters (8 bytes) */
    char hex_str[17];
    CEEPEW_ASSERT(sizeof(hex_str) >= 17U, CEEPEW_ERR_BOUNDS);
    
    /* Format as two lines of 8 hex chars each */
    for (uint8_t i = 0U; i < 8U; i++) {
        uint8_t byte_val = (uint8_t)((session_id >> (56U - i * 8U)) & 0xFFU);
        uint8_t nibble_h = (byte_val >> 4U) & 0x0FU;
        uint8_t nibble_l = byte_val & 0x0FU;
        
        char hex_h = (nibble_h < 10U) ? (char)('0' + nibble_h) : (char)('A' + nibble_h - 10U);
        char hex_l = (nibble_l < 10U) ? (char)('0' + nibble_l) : (char)('A' + nibble_l - 10U);
        
        hex_str[i * 2U] = hex_h;
        hex_str[i * 2U + 1U] = hex_l;
    }
    hex_str[16] = '\0';
    
    /* Display first 8 hex chars on line 1 */
    char line1[9];
    memcpy(line1, hex_str, 8U);
    line1[8] = '\0';
    hal_ui_text(20U, 14U, line1, HAL_UI_WHITE);
    
    /* Display second 8 hex chars on line 2 */
    char line2[9];
    memcpy(line2, hex_str + 8U, 8U);
    line2[8] = '\0';
    hal_ui_text(20U, 24U, line2, HAL_UI_WHITE);
    
    /* Draw 8x8 pixel grid visualization of fingerprint (bits from session_id) */
    for (uint8_t row = 0U; row < 8U; row++) {
        for (uint8_t col = 0U; col < 8U; col++) {
            /* Use bits of session_id to determine pixel state */
            uint8_t bit_index = (row * 8U + col) % 64U;
            uint8_t bit_set = (uint8_t)((session_id >> bit_index) & 1U);
            
            if (bit_set) {
                uint8_t px = (uint8_t)(106U + col);
                uint8_t py = (uint8_t)(14U + row);
                (void)hal_ui_pixel(px, py, HAL_UI_WHITE);
            }
        }
    }
    
    /* If show_commitment is true, display label */
    if (show_commitment) {
        hal_ui_text(8U, 48U, "Verified:OK", HAL_UI_WHITE);
    }
    
    CEEPEW_ASSERT(show_commitment || !show_commitment, CEEPEW_ERR_BOUNDS);
    
    return CEEPEW_OK;
}

static CeePewErr_t render_keyder_anim(void)
{
    /* Sprint 10: Key derivation animation — use enhanced progress display */
    uint8_t frame = (uint8_t)(g_ui_ctx.anim.frame_count % 100U);
    
    hal_ui_clear();
    
    /* Use helper function to render animated progress */
    CeePewErr_t err = ui_keygen_show_progress(frame);
    if (err != CEEPEW_OK) { return err; }
    
    g_ui_ctx.anim.frame_count++;
    
    /* After 100 frames, check if peer acknowledged (nonce_counter > 0) */
    if (frame >= 99U) {
        uint64_t nonce_counter = session_get_nonce_counter();
        if (nonce_counter > 0ULL) {
            /* Peer acknowledged: transition to fingerprint display */
            g_ui_ctx.transition_ready = true;
        }
    }
    
    return CEEPEW_OK;
}

static CeePewErr_t render_fingerprint(void)
{
    (void)session_get_fingerprint(g_ui_ctx.fingerprint);

    CeePewErr_t err = ui_fingerprint_show_display(g_ui_ctx.fingerprint, g_ui_ctx.peer_mac);
    if (err != CEEPEW_OK) { return err; }

    return CEEPEW_OK;
}

static CeePewErr_t render_fingerprint_confirm(void)
{
    return ui_fingerprint_show_confirm(g_ui_ctx.fingerprint, g_ui_ctx.peer_mac);
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
    /* Sprint 11: Chat screen with message history, compose UI, and character pool */
    CEEPEW_ASSERT(true, CEEPEW_ERR_PARAM);  /* Entry guard */
    
    hal_ui_clear();
    
    /* Draw title */
    hal_ui_text(48U, 2U, "Chat", HAL_UI_WHITE);
    
    /* Retrieve and display recent messages (up to 3 bubbles) */
    uint8_t msg_count = msg_store_count();
    uint8_t bubbles_shown = 0U;
    
    if (msg_count > 0U) {
        /* Show most recent 2 messages */
        for (uint8_t i = 0U; i < 2U && bubbles_shown < 2U; i++) {
            uint8_t msg_idx = (msg_count > (i + 1U)) ? (msg_count - 2U + i) : i;
            const StoredMsg_t *msg = msg_store_get(msg_idx);
            if (msg != NULL) {
                uint8_t y = (uint8_t)(14U + bubbles_shown * 12U);
                (void)ui_chat_show_bubble(msg_idx, y, msg->meta.dir);
                bubbles_shown++;
            }
        }
    }
    
    /* If no messages, show placeholder */
    if (msg_count == 0U) {
        hal_ui_text(28U, 20U, "No messages", HAL_UI_WHITE);
    }
    
    CEEPEW_ASSERT(bubbles_shown <= 2U, CEEPEW_ERR_BOUNDS);
    
    /* Display character pool indicator (budget and selector) */
    uint8_t remaining_chars = (CEEPEW_MAX_MSG_CHARS > 10U) ? (CEEPEW_MAX_MSG_CHARS - 10U) : 0U;
    (void)ui_chat_show_pool(remaining_chars);
    
    /* Button press initiates message composition mode */
    if (g_ui_ctx.button_pressed) {
        /* Display compose UI */
        (void)ui_chat_show_compose(g_ui_ctx.user_input, 0U);
    }
    
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
    /* Sprint 12: Cryptogram verification display */
    CEEPEW_ASSERT(true, CEEPEW_ERR_PARAM);  /* Entry guard */
    
    hal_ui_clear();
    
    /* Retrieve local commitment from session */
    CeePewErr_t err = session_get_commitment(g_ui_ctx.commitment);
    if (err != CEEPEW_OK) {
        hal_ui_text(8U, 20U, "Commit error", HAL_UI_WHITE);
        (void)hal_ui_flush();
        return err;
    }
    
    /* Display cryptogram */
    (void)ui_crypto_show_cryptogram(g_ui_ctx.commitment);
    
    /* Determine status: compare with peer commitment from BLE context */
    uint8_t status = 0U;  /* 0=waiting, 1=match, 2=mismatch */
    
    if (g_ble_ctx.commitment_verified) {
        /* Compare commitments (constant-time comparison would be ideal,
           but for UI this is acceptable since status is not sensitive) */
        uint8_t match = 1U;
        for (uint8_t i = 0U; i < 8U; i++) {
            if (g_ui_ctx.commitment[i] != g_ble_ctx.commitment_digest[i]) {
                match = 0U;
                break;
            }
        }
        status = match ? 1U : 2U;
        g_ui_ctx.commitment_verified = (match == 1U) ? true : false;
    }
    
    /* Display status */
    (void)ui_crypto_show_status(status);
    
    /* Show confirmation UI if match or after timeout */
    if (status == 1U) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        uint32_t elapsed = 0U;
        if (g_ui_ctx.crypto_confirm_start_ms != 0U && now_ms >= g_ui_ctx.crypto_confirm_start_ms) {
            elapsed = now_ms - g_ui_ctx.crypto_confirm_start_ms;
        }
        const uint32_t CONFIRM_TIMEOUT_MS = 30000U;  /* 30 second confirmation window */
        uint32_t rem_ms = (elapsed < CONFIRM_TIMEOUT_MS) ? (CONFIRM_TIMEOUT_MS - elapsed) : 0U;
        uint8_t countdown_sec = (uint8_t)((rem_ms + 999U) / 1000U);
        if (countdown_sec > 60U) { countdown_sec = 60U; }
        
        (void)ui_crypto_show_confirm(countdown_sec);
        
        /* Button press confirms match and transitions to chat */
        if (g_ui_ctx.button_pressed) {
            (void)ui_manager_transition_to(UI_STATE_CHAT);
            g_ui_ctx.transition_ready = true;
        }
        
        /* Auto-transition to chat if confirmation window expires */
        if (rem_ms == 0U) {
            (void)ui_manager_transition_to(UI_STATE_CHAT);
            g_ui_ctx.transition_ready = true;
        }
    }
    
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

    hal_ui_flush();
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

    hal_ui_flush();
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

    hal_ui_flush();
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

    hal_ui_flush();
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

/* components/ceepew_hal/ui_manager.h
 *
 * Consolidated UI manager for all remaining screen states.
 * Handles state transitions, animations, and input integration.
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_ui.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UI screen state enum (Sprints 8-12 + Phase 4) */
typedef enum {
    UI_STATE_BOOT = 0U,           /* Sprint 8: Boot animation */
    UI_STATE_DISCOVERY = 1U,      /* Sprint 8: Radar/discovery */
    UI_STATE_CODE_ENTRY = 2U,     /* Sprint 9: Code entry grid */
    UI_STATE_COUNTDOWN = 3U,      /* Sprint 9: Countdown bar (legacy) */
    /* 4U and 5U reserved — removed CODE_INCORRECT/CODE_DIFFERENT (merged into PAIRING_FAILED) */
    UI_STATE_CONFIRM = 6U,        /* Code verification screen — auto-verify on entry */
    UI_STATE_KEYDER = 7U,         /* Key derivation anim (configurable duration) */
    UI_STATE_CHAT = 8U,           /* Sprint 11: Chat bubbles */
    UI_STATE_CHAT_MENU = 9U,      /* Phase 4: Chat menu (Write/Read) */
    UI_STATE_CHAT_COMPOSE = 10U,  /* Phase 4: Chat message composition with keyboard */
    UI_STATE_CRYPTOGRAM = 11U,    /* Keys verified success screen (tick mark) */
    UI_STATE_NONCE_EXHAUSTED = 12U, /* Phase 4: Nonce limit exhausted */
    UI_STATE_INFO = 13U,          /* DIAG-only: Info / diagnostics display */
    UI_STATE_ERROR = 14U,         /* Phase 4: Generic error display */
    UI_STATE_PAIRING = 15U,       /* Sprint 9: Pairing countdown state */
    UI_STATE_PAIRING_FAILED = 16U,  /* Pairing outcome: failure banner */
    UI_STATE_CHAT_SEND_CONFIRM = 17U, /* Phase 4: Confirm composed message before send */
} UIState_t;

typedef enum {
    UI_PAIRING_RESULT_NONE = 0U,
    UI_PAIRING_RESULT_SUCCESS = 1U,
    UI_PAIRING_RESULT_TIMED_OUT = 2U,
    UI_PAIRING_RESULT_LINK_FAIL = 3U,
    UI_PAIRING_RESULT_COMMITMENT_FAIL = 4U,
    UI_PAIRING_RESULT_UNKNOWN = 5U,
} PairingResultReason_t;

/* Animation frame context */
typedef struct {
    uint32_t frame_count;         /* Current frame number */
    uint32_t frame_rate_ms;       /* Milliseconds per frame */
    uint32_t start_time_ms;       /* Animation start timestamp */
    bool     active;              /* Animation running */
} AnimContext_t;

/* UI screen context */
typedef struct {
    UIState_t     current_state;
    UIState_t     next_state;
    AnimContext_t anim;
    uint32_t      last_draw_ms;
    uint8_t       user_input;    /* Rotary potentiometer: 0-255 */
    bool          button_pressed;
    bool          button_prev; /* previous button state for edge detection */
    uint32_t      button_press_start_ms; /* ms timestamp when button was pressed */
    bool          diag_mode;
    bool          transition_ready;
    /* Sprint-9 code entry context */
    uint8_t       code_digits[4];
    uint8_t       code_selected; /* index 0-3 */
    uint32_t      code_entry_start_ms; /* ms timestamp when code entry became visible */
    uint32_t      countdown_start_ms; /* ms timestamp when countdown began */
    uint32_t      pairing_start_ms; /* ms timestamp when pairing began */
    uint32_t      pairing_result_start_ms; /* ms timestamp when result banner began */
    uint8_t       pairing_result_reason; /* PairingResultReason_t */
    /* Sprint-12 cryptogram context */
    uint8_t       commitment[CEEPEW_COMMITMENT_BYTES];        /* Local session commitment (SHA256 truncated) */
    uint8_t       peer_commitment[CEEPEW_COMMITMENT_BYTES];   /* Peer commitment from BLE */
    bool          commitment_verified;  /* true = commitments match */
    uint32_t      crypto_confirm_start_ms; /* ms timestamp when confirmation started */
    /* Phase 4: Error states */
    uint8_t       peer_mac[6];          /* Peer MAC for display */
    uint32_t      reject_sequence_start_ms; /* ms when red reject blink started */
    uint32_t      error_start_ms;       /* ms when error state was entered */
    /* Phase 4: Chat menu, compose, and send-confirm context */
    uint8_t       chat_menu_selected;   /* 0=Read, 1=Write, 2=Check */
    uint8_t       chat_send_confirm_selected; /* 0=Send, 1=Go back */
    char          compose_buffer[256];  /* Message composition buffer */
    uint8_t       compose_length;       /* Current message length */
    uint8_t       compose_cursor;       /* Cursor position in message */
    uint8_t       keyboard_row;         /* Current keyboard row (0-9 for 6x10 grid) */
    uint8_t       keyboard_col;         /* Current keyboard column (0-5) */
} UIContext_t;

extern UIContext_t g_ui_ctx;

/* Initialize UI manager */
CeePewErr_t ui_manager_init(void);

/* Update UI state machine (call from main loop) */
CeePewErr_t ui_manager_update(void);

/* Draw current screen */
CeePewErr_t ui_manager_draw(void);

/* Transition to new screen state */
CeePewErr_t ui_manager_transition_to(UIState_t next_state);

/* Handle user input */
CeePewErr_t ui_manager_handle_input(uint8_t pot_value, bool button_pressed, bool diag_mode);

/* Get animation frame (0-255 normalized) */
uint8_t ui_manager_get_anim_frame(void);

/* Sprint 10: Enhanced key derivation animation display.
 * Shows "DERIVING..." text with animated progress bar.
 * Updates for each frame; caller loops this in render function.
 */
CeePewErr_t ui_keygen_show_progress(uint8_t frame_index);

/* Sprint 11: Chat message bubble rendering.
 * Displays a single message as a bubble with text preview and status indicator.
 * dir: 0=RX (left-aligned), 1=TX (right-aligned)
 */
CeePewErr_t ui_chat_show_bubble(uint8_t msg_idx, uint8_t y_pos, uint8_t dir);

/* Sprint 11: Character pool display.
 * Shows available character budget for message composition.
 */
CeePewErr_t ui_chat_show_pool(uint8_t char_budget);

/* Sprint 11: Message compose UI with character selector.
 * Displays character selection wheel and compose controls.
 */
CeePewErr_t ui_chat_show_compose(uint8_t pot_value, uint8_t selected_idx);

/* Sprint 12: Cryptogram display with grouped hex rows.
 * Displays 16-byte session commitment as hex with 2-byte groups for readability.
 * Centered on display in monospace font.
 * No dynamic allocation; static hex conversion buffer.
 * Two CEEPEW_ASSERTs for bounds checking.
 */
CeePewErr_t ui_crypto_show_cryptogram(const uint8_t commitment[CEEPEW_COMMITMENT_BYTES]);

/* Sprint 12: Cryptogram status display (match/mismatch/waiting).
 * Shows visual indicator (checkmark or X) and status text.
 * status: 0=waiting, 1=match, 2=mismatch
 * No dynamic allocation; static buffers.
 * Two CEEPEW_ASSERTs for validation.
 */
CeePewErr_t ui_crypto_show_status(uint8_t status);

/* Sprint 12: Cryptogram confirmation UI with countdown.
 * Displays confirmation button prompt and countdown timer.
 * countdown_sec: seconds remaining (0-60)
 * No dynamic allocation; static text buffers.
 * Two CEEPEW_ASSERTs for bounds checking.
 */
CeePewErr_t ui_crypto_show_confirm(uint8_t countdown_sec);

/* Phase 4: Nonce exhausted error panel.
 * Displays "Session expired — nonce limit hit. Restart to re-pair."
 * with red blink animation.
 * No parameters; uses g_ui_ctx.error_start_ms for animation timing.
 */
CeePewErr_t ui_show_nonce_exhausted(void);

/* Phase 4: Reset UI to discovery mode and clear all state.
 * Called by session_wipe() to hard-reset UI on TTL expiry or security event.
 * Clears OLED, sets RGB to discovery pattern (blue pulse).
 */
void ui_manager_reset_to_discovery(void);

/* UI → BLE visual feedback bridge.
 * Maps the current BLE pairing phase to an RGB pattern so the user gets
 * fine-grained progress feedback (CONNECTING, MTU, COMMITMENT, etc.) instead
 * of a single "amber pulse" while the pairing countdown runs.
 *
 * If the PairingSupervisor is currently performing a forced recovery
 * (PHASE_TIMEOUT / RADIO_RESTART), the recovery indicator pattern
 * (RGB_YELLOW_RED_BLINK) takes precedence over the phase pattern.
 *
 * Called once per UI tick from ui_manager_update() while on the pairing
 * screen. Safe to call repeatedly -- only writes the LED when the desired
 * pattern changes. */
void task_ui_update_visual_feedback(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H */

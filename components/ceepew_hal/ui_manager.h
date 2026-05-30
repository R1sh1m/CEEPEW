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
    UI_STATE_COUNTDOWN = 3U,      /* Sprint 9: Countdown bar */
    UI_STATE_CODE_INCORRECT = 4U, /* New: Code mismatch UI */
    UI_STATE_CODE_DIFFERENT = 5U, /* New: Different code UI */
    UI_STATE_CONFIRM = 6U,        /* Sprint 9: Confirmation */
    UI_STATE_KEYDER = 7U,         /* Sprint 10: Key derivation anim */
    UI_STATE_FINGERPRINT = 8U,    /* Sprint 10: Fingerprint display */
    UI_STATE_FINGERPRINT_CONFIRM = 9U,  /* Phase 4: Confirm fingerprint with D/S */
    UI_STATE_CHAT = 10U,           /* Sprint 11: Chat bubbles */
    UI_STATE_CHAT_MENU = 11U,      /* Phase 4: Chat menu (Read/Write/Check) */
    UI_STATE_CHAT_COMPOSE = 12U,   /* Phase 4: Chat message composition with keyboard */
    UI_STATE_CRYPTOGRAM = 13U,     /* Sprint 12: Cryptogram panel */
    UI_STATE_NONCE_EXHAUSTED = 14U, /* Phase 4: Nonce limit exhausted */
    UI_STATE_INFO = 15U,           /* DIAG-only: Info / diagnostics display */
    UI_STATE_ERROR = 16U,         /* Phase 4: Generic error display */
    UI_STATE_PAIRING = 17U,       /* Sprint 9: Pairing countdown state (legacy name) */
    UI_STATE_PAIRING_SUCCESS = 18U, /* Pairing outcome: success banner */
    UI_STATE_PAIRING_FAILED = 19U,  /* Pairing outcome: failure banner */
    UI_STATE_CHAT_SEND_CONFIRM = 20U, /* Phase 4: Confirm composed message before send */
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
    /* Phase 4: Fingerprint confirmation and error states */
    uint8_t       fingerprint[16];      /* Device fingerprint for confirmation */
    uint8_t       peer_mac[6];          /* Peer MAC for display */
    bool          fingerprint_confirmed; /* true if user confirmed (D button) */
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

/* Sprint 10: Fingerprint display with session ID and visual grid.
 * Renders session_id as 16 hex digits (8x2 layout) plus 8x8 pixel grid.
 * Displays commitment value label if available.
 * param: show_commitment: set to true to display commitment label
 */
CeePewErr_t ui_keygen_show_fingerprint(bool show_commitment);

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

/* Sprint 12: Cryptogram display with four 8-character hex rows (32 hex chars total).
 * Displays 16-byte session commitment as hex (session_code SHA256 digest).
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

/* Phase 4: Fingerprint confirmation panel display.
 * Shows 16-byte fingerprint in hex, peer MAC, and D=Accept/S=Reject prompts.
 * fingerprint: 16 bytes to display (not NULL)
 * peer_mac: 6-byte MAC for identification (not NULL)
 * No dynamic allocation; static hex conversion buffer.
 * Two CEEPEW_ASSERTs for null/bounds checking.
 */
CeePewErr_t ui_fingerprint_show_confirm(const uint8_t fingerprint[16],
                                        const uint8_t peer_mac[6]);

/* Phase 4: Fingerprint display panel.
 * Shows 16-byte fingerprint in hex with peer MAC before confirmation.
 */
CeePewErr_t ui_fingerprint_show_display(const uint8_t fingerprint[16],
                                        const uint8_t peer_mac[6]);

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

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H */

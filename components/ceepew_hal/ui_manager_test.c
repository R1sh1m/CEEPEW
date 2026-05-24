/* components/ceepew_hal/ui_manager_test.c
 * Headless unit tests for ui_manager (Sprint 9 & 10)
 * Guarded by CEEPEW_ENABLE_SELFTEST to only run when explicitly enabled.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ui_manager.h"
#include "hal_ui.h"
#include "../transport/transport_ble.h"

#ifdef CEEPEW_ENABLE_SELFTEST

/* Mock/stub implementations for session FSM functions (Sprint 10) */
__attribute__((weak)) uint64_t session_get_id(void) {
    /* Return a test session ID for fingerprint rendering */
    return 0x0123456789ABCDEFULL;
}

__attribute__((weak)) uint64_t session_get_nonce_counter(void) {
    /* Return 0 initially, can be mocked to test commitment verification */
    return 0ULL;
}

__attribute__((constructor)) static void ui_manager_selftest(void) {
    printf("CEEPEW: ui_manager selftest start\n");

    /* Initialize UI and HAL */
    hal_ui_init();
    if (ui_manager_init() != CEEPEW_OK) {
        printf("ui_manager_init failed\n");
        return;
    }
    
    /* Initialize message store for chat tests */
    if (msg_store_init() != CEEPEW_OK) {
        printf("ui_manager selftest: msg_store_init failed\n");
        return;
    }

    /* Test 1: Transition to code entry screen */
    (void)ui_manager_transition_to(UI_STATE_CODE_ENTRY);
    g_ui_ctx.transition_ready = true;
    (void)ui_manager_update();

    if (g_ui_ctx.current_state != UI_STATE_CODE_ENTRY) {
        printf("ui_manager selftest: failed to enter CODE_ENTRY\n");
        return;
    }
    printf("CEEPEW: ui_manager selftest - CODE_ENTRY PASS\n");

    /* Simulate entering digits 1,2,3,4 using pot + short press */
    uint8_t digits[4] = {1,2,3,4};
    for (uint8_t i = 0; i < 4; i++) {
        /* Compute pot value that maps to desired digit: pot*10/256 ~= digit */
        uint8_t pot = (uint8_t)((((uint16_t)digits[i] * 256U) / 10U) & 0xFFU);
        ui_manager_handle_input(pot, false, false);
        ui_manager_update();

        /* Press and release quickly (short press) */
        ui_manager_handle_input(pot, true, false);
        ui_manager_update();
        /* Simulate release */
        ui_manager_handle_input(pot, false, false);
        ui_manager_update();
    }

    /* After entering 4 digits, confirm with press-and-hold (simulate long hold) */
    uint8_t pot = (uint8_t)((((uint16_t)5U * 256U) / 10U) & 0xFFU);
    /* Press */
    ui_manager_handle_input(pot, true, false);
    /* Force press start timestamp to simulate long hold */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    g_ui_ctx.button_press_start_ms = now_ms - 1500U; /* held for 1.5s */
    /* Release */
    ui_manager_handle_input(pot, false, false);
    ui_manager_update();

    /* ui_manager should have transitioned to COUNTDOWN */
    if (g_ui_ctx.current_state != UI_STATE_COUNTDOWN) {
        printf("ui_manager selftest: failed to enter COUNTDOWN (state=%d)\n", (int)g_ui_ctx.current_state);
        return;
    }
    printf("CEEPEW: ui_manager selftest - COUNTDOWN PASS\n");

    /* Simulate BLE commitment verified to advance early to CONFIRM */
    g_ble_ctx.commitment_verified = true;
    ui_manager_update();

    if (g_ui_ctx.current_state != UI_STATE_CONFIRM) {
        printf("ui_manager selftest: failed to enter CONFIRM after BLE verify (state=%d)\n", (int)g_ui_ctx.current_state);
        return;
    }
    printf("CEEPEW: ui_manager selftest - CONFIRM PASS\n");

    /* Test 2: Sprint 10 - Transition to key derivation (KEYDER) */
    g_ui_ctx.button_pressed = true;
    ui_manager_update();
    g_ui_ctx.button_pressed = false;
    
    if (g_ui_ctx.current_state != UI_STATE_KEYDER) {
        printf("ui_manager selftest: failed to enter KEYDER (state=%d)\n", (int)g_ui_ctx.current_state);
        return;
    }
    printf("CEEPEW: ui_manager selftest - KEYDER transition PASS\n");

    /* Test 3: Test ui_keygen_show_progress() rendering */
    for (uint8_t frame = 0; frame < 100U; frame++) {
        if (ui_keygen_show_progress(frame) != CEEPEW_OK) {
            printf("ui_manager selftest: ui_keygen_show_progress(%u) failed\n", frame);
            return;
        }
    }
    printf("CEEPEW: ui_manager selftest - ui_keygen_show_progress PASS\n");

    /* Simulate rendering multiple KEYDER frames */
    for (uint8_t frame = 0; frame < 20; frame++) {
        if (ui_manager_draw() != CEEPEW_OK) {
            printf("ui_manager selftest: ui_manager_draw() failed on frame %u\n", frame);
            return;
        }
        (void)ui_manager_update();
    }
    printf("CEEPEW: ui_manager selftest - KEYDER rendering PASS\n");

    /* Test 4: Transition to FINGERPRINT when nonce_counter > 0 */
    g_ui_ctx.transition_ready = true;
    (void)ui_manager_transition_to(UI_STATE_FINGERPRINT);
    (void)ui_manager_update();

    if (g_ui_ctx.current_state != UI_STATE_FINGERPRINT) {
        printf("ui_manager selftest: failed to enter FINGERPRINT (state=%d)\n", (int)g_ui_ctx.current_state);
        return;
    }
    printf("CEEPEW: ui_manager selftest - FINGERPRINT transition PASS\n");

    /* Test 5: Test ui_keygen_show_fingerprint() rendering */
    if (ui_keygen_show_fingerprint(false) != CEEPEW_OK) {
        printf("ui_manager selftest: ui_keygen_show_fingerprint(false) failed\n");
        return;
    }
    printf("CEEPEW: ui_manager selftest - ui_keygen_show_fingerprint(false) PASS\n");

    if (ui_keygen_show_fingerprint(true) != CEEPEW_OK) {
        printf("ui_manager selftest: ui_keygen_show_fingerprint(true) failed\n");
        return;
    }
    printf("CEEPEW: ui_manager selftest - ui_keygen_show_fingerprint(true) PASS\n");

    /* Simulate rendering fingerprint screen */
    for (uint8_t frame = 0; frame < 10; frame++) {
        if (ui_manager_draw() != CEEPEW_OK) {
            printf("ui_manager selftest: ui_manager_draw() failed on FINGERPRINT frame %u\n", frame);
            return;
        }
        (void)ui_manager_update();
    }
    printf("CEEPEW: ui_manager selftest - FINGERPRINT rendering PASS\n");

    /* Test 6: Fingerprint screen with button press should transition to CHAT */
    g_ui_ctx.button_pressed = true;
    (void)ui_manager_draw();
    (void)ui_manager_update();
    g_ui_ctx.button_pressed = false;

    if (g_ui_ctx.next_state != UI_STATE_CHAT) {
        printf("ui_manager selftest: fingerprint button should transition to CHAT (next_state=%d)\n", (int)g_ui_ctx.next_state);
        return;
    }
    printf("CEEPEW: ui_manager selftest - FINGERPRINT button transition PASS\n");

    /* Test 7: Sprint 11 - Transition to CHAT state */
    (void)ui_manager_transition_to(UI_STATE_CHAT);
    g_ui_ctx.transition_ready = true;
    (void)ui_manager_update();
    
    if (g_ui_ctx.current_state != UI_STATE_CHAT) {
        printf("ui_manager selftest: failed to enter CHAT (state=%d)\n", (int)g_ui_ctx.current_state);
        return;
    }
    printf("CEEPEW: ui_manager selftest - CHAT transition PASS\n");
    
    /* Test 8: Test ui_chat_show_bubble() with mock messages */
    if (msg_store_count() == 0U) {
        /* Add a test message for display */
        uint8_t test_payload[20] = "Hello from peer!    ";
        (void)msg_store_add(test_payload, 20U, 15U, 0U);  /* RX message */
    }
    
    if (ui_chat_show_bubble(0U, 14U, 0U) != CEEPEW_OK) {
        printf("ui_manager selftest: ui_chat_show_bubble() failed\n");
        return;
    }
    printf("CEEPEW: ui_manager selftest - ui_chat_show_bubble PASS\n");
    
    /* Test 9: Test ui_chat_show_pool() */
    if (ui_chat_show_pool(150U) != CEEPEW_OK) {
        printf("ui_manager selftest: ui_chat_show_pool() failed\n");
        return;
    }
    printf("CEEPEW: ui_manager selftest - ui_chat_show_pool PASS\n");
    
    /* Test 10: Test ui_chat_show_compose() with various pot values */
    for (uint8_t pot = 0U; pot < 255U; pot += 32U) {
        if (ui_chat_show_compose(pot, 0U) != CEEPEW_OK) {
            printf("ui_manager selftest: ui_chat_show_compose(pot=%u) failed\n", pot);
            return;
        }
    }
    printf("CEEPEW: ui_manager selftest - ui_chat_show_compose PASS\n");
    
    /* Test 11: Simulate rendering CHAT screen multiple times */
    for (uint8_t frame = 0; frame < 15; frame++) {
        if (ui_manager_draw() != CEEPEW_OK) {
            printf("ui_manager selftest: ui_manager_draw() failed on CHAT frame %u\n", frame);
            return;
        }
        (void)ui_manager_update();
    }
    printf("CEEPEW: ui_manager selftest - CHAT rendering PASS\n");
    
    /* Test 12: Test compose mode with button press */
    g_ui_ctx.button_pressed = true;
    g_ui_ctx.user_input = 128U;  /* Mid-range pot value */
    if (ui_manager_draw() != CEEPEW_OK) {
        printf("ui_manager selftest: ui_manager_draw() with compose failed\n");
        return;
    }
    g_ui_ctx.button_pressed = false;
    printf("CEEPEW: ui_manager selftest - CHAT compose mode PASS\n");

    printf("CEEPEW: ui_manager selftest PASS (all tests passed)\n");
}

#endif /* CEEPEW_ENABLE_SELFTEST */


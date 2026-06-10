/* components/ceepew_hal/ui_cryptogram_test.c
 * Unit tests for Sprint 12 cryptogram UI implementation.
 * Guarded by CEEPEW_ENABLE_SELFTEST to only run when explicitly enabled.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ui_manager.h"
#include "hal_ui.h"
#include "session_msgstore.h"
#include "../transport/transport_ble.h"
#include "session_fsm.h"

#ifdef CEEPEW_ENABLE_SELFTEST

void ui_cryptogram_selftest_run(void)
{
    printf("CEEPEW: ui_cryptogram selftest start\n");

    /* Inject a deterministic test commitment for display. Replaces the old
     * __attribute__((weak)) test stub. */
    static const uint8_t test_commitment[CEEPEW_COMMITMENT_BYTES] = {
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
    };
    session_test_set_commitment(test_commitment);

    /* Initialize UI and HAL */
    hal_ui_init();
    if (ui_manager_init() != CEEPEW_OK) {
        printf("ui_cryptogram selftest: ui_manager_init failed\n");
        return;
    }

    /* Initialize message store for chat integration */
    if (msg_store_init() != CEEPEW_OK) {
        printf("ui_cryptogram selftest: msg_store_init failed\n");
        return;
    }

    /* Test 1: Transition to CRYPTOGRAM state */

    /* Test 1: Transition to CRYPTOGRAM state */
    (void)ui_manager_transition_to(UI_STATE_CRYPTOGRAM);
    g_ui_ctx.transition_ready = true;
    (void)ui_manager_update();

    if (g_ui_ctx.current_state != UI_STATE_CRYPTOGRAM) {
        printf("ui_cryptogram selftest: failed to enter CRYPTOGRAM state (state=%d)\n",
               (int)g_ui_ctx.current_state);
        return;
    }
    printf("CEEPEW: ui_cryptogram selftest - CRYPTOGRAM transition PASS\n");

    /* Test 2: Test ui_crypto_show_cryptogram() */
    if (ui_crypto_show_cryptogram(test_commitment) != CEEPEW_OK) {
        printf("ui_cryptogram selftest: ui_crypto_show_cryptogram() failed\n");
        return;
    }
    printf("CEEPEW: ui_cryptogram selftest - ui_crypto_show_cryptogram PASS\n");

    /* Test 3: Test ui_crypto_show_status() with all states */
    for (uint8_t status = 0U; status <= 2U; status++) {
        if (ui_crypto_show_status(status) != CEEPEW_OK) {
            printf("ui_cryptogram selftest: ui_crypto_show_status(%u) failed\n", status);
            return;
        }
    }
    printf("CEEPEW: ui_cryptogram selftest - ui_crypto_show_status PASS\n");

    /* Test 4: Test ui_crypto_show_confirm() with countdown values */
    for (uint8_t countdown = 0U; countdown <= 30U; countdown += 5U) {
        if (ui_crypto_show_confirm(countdown) != CEEPEW_OK) {
            printf("ui_cryptogram selftest: ui_crypto_show_confirm(%u) failed\n", countdown);
            return;
        }
    }
    printf("CEEPEW: ui_cryptogram selftest - ui_crypto_show_confirm PASS\n");

    /* Test 5: Simulate rendering cryptogram screen multiple times */
    for (uint8_t frame = 0U; frame < 20U; frame++) {
        if (ui_manager_draw() != CEEPEW_OK) {
            printf("ui_cryptogram selftest: ui_manager_draw() failed on frame %u\n", frame);
            return;
        }
        (void)ui_manager_update();
    }
    printf("CEEPEW: ui_cryptogram selftest - CRYPTOGRAM rendering PASS\n");

    /* Test 6: Simulate peer commitment verification */
    g_ble_ctx.commitment_verified = true;
    memcpy(g_ble_ctx.commitment_digest, test_commitment, CEEPEW_COMMITMENT_BYTES);
    if (ui_manager_draw() != CEEPEW_OK) {
        printf("ui_cryptogram selftest: ui_manager_draw() with peer verification failed\n");
        return;
    }
    printf("CEEPEW: ui_cryptogram selftest - CRYPTOGRAM peer verification PASS\n");

    /* Test 7: Test mismatch scenario */
    g_ble_ctx.commitment_verified = true;
    uint8_t different_commitment[CEEPEW_COMMITMENT_BYTES] = {
        0xF0, 0xED, 0xCB, 0xA9, 0x87, 0x65, 0x43, 0x21,
        0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE
    };
    memcpy(g_ble_ctx.commitment_digest, different_commitment, CEEPEW_COMMITMENT_BYTES);
    if (ui_manager_draw() != CEEPEW_OK) {
        printf("ui_cryptogram selftest: ui_manager_draw() with mismatch failed\n");
        return;
    }
    printf("CEEPEW: ui_cryptogram selftest - CRYPTOGRAM mismatch PASS\n");

    /* Test 8: Test button press transitions to CHAT */
    g_ble_ctx.commitment_verified = true;
    memcpy(g_ble_ctx.commitment_digest, test_commitment, CEEPEW_COMMITMENT_BYTES);
    g_ui_ctx.button_pressed = true;
    (void)ui_manager_draw();
    (void)ui_manager_update();
    g_ui_ctx.button_pressed = false;

    if (g_ui_ctx.next_state != UI_STATE_CHAT) {
        printf("ui_cryptogram selftest: button press should transition to CHAT (next_state=%d)\n",
               (int)g_ui_ctx.next_state);
        return;
    }
    printf("CEEPEW: ui_cryptogram selftest - CRYPTOGRAM button transition PASS\n");

    /* Test 9: Verify cryptogram context initialization on state entry */
    (void)ui_manager_transition_to(UI_STATE_CRYPTOGRAM);
    g_ui_ctx.transition_ready = true;
    (void)ui_manager_update();

    if (g_ui_ctx.current_state != UI_STATE_CRYPTOGRAM) {
        printf("ui_cryptogram selftest: re-entry to CRYPTOGRAM failed\n");
        return;
    }

    if (g_ui_ctx.crypto_confirm_start_ms == 0U) {
        printf("ui_cryptogram selftest: crypto_confirm_start_ms not initialized\n");
        return;
    }
    printf("CEEPEW: ui_cryptogram selftest - CRYPTOGRAM state re-entry PASS\n");

    /* Test 10: Verify rendering without BLE context (waiting state) */
    g_ble_ctx.commitment_verified = false;
    if (ui_manager_draw() != CEEPEW_OK) {
        printf("ui_cryptogram selftest: ui_manager_draw() in waiting state failed\n");
        return;
    }
    printf("CEEPEW: ui_cryptogram selftest - CRYPTOGRAM waiting state PASS\n");

    printf("CEEPEW: ui_cryptogram selftest PASS (all tests passed)\n");
}

#endif /* CEEPEW_ENABLE_SELFTEST */

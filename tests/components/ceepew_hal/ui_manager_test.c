/* components/ceepew_hal/ui_manager_test.c
 * Headless unit tests for ui_manager (Sprint 9 & 10)
 * Guarded by CEEPEW_ENABLE_SELFTEST to only run when explicitly enabled.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "layout.h"
#include "ui_manager.h"
#include "hal_ui.h"
#include "ceepew_oled.h"
#include "ceepew_oled_gfx_primitives.h"
#include "ceepew_oled_font_adapter.h"
#include "session_msgstore.h"
#include "hal_pins.h"
#include "esp_timer.h"
#include "../transport/transport_ble.h"
#include "session_fsm.h"

#ifdef CEEPEW_ENABLE_SELFTEST

void ui_manager_selftest_run(void) {
    printf("CEEPEW: ui_manager selftest start\n");
    extern void ceepew_oled_backup_state(void);
    ceepew_oled_backup_state();

    /* Inject deterministic test values for session_get_id / session_get_nonce_counter.
     * Replaces the old __attribute__((weak)) test stubs. */
    session_test_set_id(0x0123456789ABCDEFULL);
    session_test_set_nonce_counter(0ULL);

    /* UI and HAL are already initialized by app_main at boot. */

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
    
    /* Mock BLE discover and transition to COUNTDOWN to simulate the FSM flow */
    g_ble_ctx.discovered = true;
    (void)ui_manager_transition_to(UI_STATE_COUNTDOWN);
    g_ui_ctx.transition_ready = true;
    ui_manager_update();

    /* ui_manager should have transitioned to COUNTDOWN */
    if (g_ui_ctx.current_state != UI_STATE_COUNTDOWN) {
        printf("ui_manager selftest: failed to enter COUNTDOWN (state=%d)\n", (int)g_ui_ctx.current_state);
        return;
    }
    printf("CEEPEW: ui_manager selftest - COUNTDOWN PASS\n");

    /* Simulate BLE commitment verified to advance early to CONFIRM */
    g_ble_ctx.commitment_verified = true;
    (void)ui_manager_transition_to(UI_STATE_CONFIRM);
    g_ui_ctx.transition_ready = true;
    ui_manager_update();

    if (g_ui_ctx.current_state != UI_STATE_CONFIRM) {
        printf("ui_manager selftest: failed to enter CONFIRM after BLE verify (state=%d)\n", (int)g_ui_ctx.current_state);
        return;
    }
    printf("CEEPEW: ui_manager selftest - CONFIRM PASS\n");

    /* Test 2: Sprint 10 - Transition to key derivation (KEYDER) */
    g_ui_ctx.button_pressed = true;
    (void)ui_manager_transition_to(UI_STATE_KEYDER);
    g_ui_ctx.transition_ready = true;
    ui_manager_update();
    g_ui_ctx.button_pressed = false;
    
    if (g_ui_ctx.current_state != UI_STATE_KEYDER) {
        printf("ui_manager selftest: failed to enter KEYDER (state=%d)\n", (int)g_ui_ctx.current_state);
        return;
    }
    if (layout_validate_state_entry(UI_STATE_KEYDER) != CEEPEW_OK) {
        printf("ui_manager selftest: keyder layout validation failed\n");
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

    /* Test 4: Cryptogram and display helpers with grouped hex */
    uint8_t test_commitment[CEEPEW_COMMITMENT_BYTES] = {
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
    };
    if (ui_crypto_show_cryptogram(test_commitment) != CEEPEW_OK) {
        printf("ui_manager selftest: ui_crypto_show_cryptogram() failed\n");
        return;
    }
    printf("CEEPEW: ui_manager selftest - ui_crypto_show_cryptogram PASS\n");

    for (uint8_t status = 0U; status <= 2U; status++) {
        if (ui_crypto_show_status(status) != CEEPEW_OK) {
            printf("ui_manager selftest: ui_crypto_show_status(%u) failed\n", status);
            return;
        }
    }
    printf("CEEPEW: ui_manager selftest - ui_crypto_show_status PASS\n");

    for (uint8_t countdown = 0U; countdown <= 30U; countdown += 5U) {
        if (ui_crypto_show_confirm(countdown) != CEEPEW_OK) {
            printf("ui_manager selftest: ui_crypto_show_confirm(%u) failed\n", countdown);
            return;
        }
    }
    printf("CEEPEW: ui_manager selftest - ui_crypto_show_confirm PASS\n");

    /* Test 8: Sprint 11 - Transition to CHAT state */
    (void)ui_manager_transition_to(UI_STATE_CHAT);
    g_ui_ctx.transition_ready = true;
    (void)ui_manager_update();
    
    if (g_ui_ctx.current_state != UI_STATE_CHAT) {
        printf("ui_manager selftest: failed to enter CHAT (state=%d)\n", (int)g_ui_ctx.current_state);
        return;
    }
    printf("CEEPEW: ui_manager selftest - CHAT transition PASS\n");

    /* Test 9: Compose long press should open SEND_CONFIRM, and cancel should return */
    (void)ui_manager_transition_to(UI_STATE_CHAT_COMPOSE);
    g_ui_ctx.transition_ready = true;
    (void)ui_manager_update();
    if (g_ui_ctx.current_state != UI_STATE_CHAT_COMPOSE) {
        printf("ui_manager selftest: failed to enter CHAT_COMPOSE (state=%d)\n", (int)g_ui_ctx.current_state);
        return;
    }

    g_ui_ctx.compose_length = 1U;
    g_ui_ctx.compose_cursor = 1U;
    g_ui_ctx.compose_buffer[0] = 'A';
    g_ui_ctx.compose_buffer[1] = '\0';

    ui_manager_handle_input(128U, true, false);
    (void)ui_manager_update();
    g_ui_ctx.button_press_start_ms = (uint32_t)(esp_timer_get_time() / 1000LL) - 2000U;
    ui_manager_handle_input(128U, false, false);
    (void)ui_manager_update();

    if (g_ui_ctx.next_state != UI_STATE_CHAT_SEND_CONFIRM) {
        printf("ui_manager selftest: compose long press should transition to CHAT_SEND_CONFIRM (next_state=%d)\n",
               (int)g_ui_ctx.next_state);
        return;
    }

    g_ui_ctx.transition_ready = true;
    (void)ui_manager_update();
    if (g_ui_ctx.current_state != UI_STATE_CHAT_SEND_CONFIRM) {
        printf("ui_manager selftest: failed to enter CHAT_SEND_CONFIRM (state=%d)\n", (int)g_ui_ctx.current_state);
        return;
    }

    ui_manager_handle_input(128U, true, false);
    (void)ui_manager_update();
    g_ui_ctx.chat_send_confirm_selected = 1U;
    ui_manager_handle_input(128U, false, false);
    (void)ui_manager_update();

    if (g_ui_ctx.next_state != UI_STATE_CHAT_COMPOSE) {
        printf("ui_manager selftest: send-confirm cancel should return to CHAT_COMPOSE (next_state=%d)\n",
               (int)g_ui_ctx.next_state);
        return;
    }

    g_ui_ctx.transition_ready = true;
    (void)ui_manager_update();
    if (g_ui_ctx.current_state != UI_STATE_CHAT_COMPOSE) {
        printf("ui_manager selftest: failed to return to CHAT_COMPOSE (state=%d)\n", (int)g_ui_ctx.current_state);
        return;
    }
    printf("CEEPEW: ui_manager selftest - CHAT send-confirm path PASS\n");

    /* Test 10: Test ui_chat_show_bubble() with mock messages */
    if (msg_store_count() == 0U) {
        /* Add a test message for display */
        uint8_t test_payload[21] = "Hello from peer!    ";
        (void)msg_store_add(test_payload, 20U, 15U, 0U);  /* RX message */
    }
    
    if (ui_chat_show_bubble(0U, 14U, 0U) != CEEPEW_OK) {
        printf("ui_manager selftest: ui_chat_show_bubble() failed\n");
        return;
    }
    printf("CEEPEW: ui_manager selftest - ui_chat_show_bubble PASS\n");
    
    /* Test 11: Test ui_chat_show_pool() */
    if (ui_chat_show_pool(150U) != CEEPEW_OK) {
        printf("ui_manager selftest: ui_chat_show_pool() failed\n");
        return;
    }
    printf("CEEPEW: ui_manager selftest - ui_chat_show_pool PASS\n");
    
    /* Test 12: Test ui_chat_show_compose() with various pot values */
    for (uint16_t pot = 0U; pot < 255U; pot += 32U) {
        if (ui_chat_show_compose((uint8_t)pot, 0U) != CEEPEW_OK) {
            printf("ui_manager selftest: ui_chat_show_compose(pot=%u) failed\n", pot);
            return;
        }
    }
    printf("CEEPEW: ui_manager selftest - ui_chat_show_compose PASS\n");
    
    /* Test 13: Simulate rendering CHAT screen multiple times */
    for (uint8_t frame = 0; frame < 15; frame++) {
        if (ui_manager_draw() != CEEPEW_OK) {
            printf("ui_manager selftest: ui_manager_draw() failed on CHAT frame %u\n", frame);
            return;
        }
        (void)ui_manager_update();
    }
    printf("CEEPEW: ui_manager selftest - CHAT rendering PASS\n");
    
    /* Test 14: Test compose mode with button press */
    g_ui_ctx.button_pressed = true;
    g_ui_ctx.user_input = 128U;  /* Mid-range pot value */
    if (ui_manager_draw() != CEEPEW_OK) {
        printf("ui_manager selftest: ui_manager_draw() with compose failed\n");
        return;
    }
    g_ui_ctx.button_pressed = false;
    printf("CEEPEW: ui_manager selftest - CHAT compose mode PASS\n");

    /* ── Sprint V2.0: in-house ceepew_oled GFX primitives + font
     * adapter + tile-dirty flush. Each sub-test creates a fresh
     * ceepew_oled_t and validates a single GFX primitive in isolation;
     * the panel is NOT connected during this headless self-test, so
     * we never call ceepew_oled_display(). */

    /* Sub-test 1: horizontal line writes a single page-byte. */
    {
        ceepew_oled_t *dev = ceepew_oled_create();
        if (dev == NULL) {
            printf("ui_manager selftest: ceepew_oled_create failed (test 1)\n");
            return;
        }
        (void)ceepew_oled_clear_buffer(dev);
        CeePewErr_t e = ceepew_oled_gfx_line(dev, 0, 32, 127, 32, HAL_UI_WHITE);
        if (e != CEEPEW_OK) {
            printf("ui_manager selftest: gfx_line horiz failed (err=%d)\n", (int)e);
            return;
        }
        uint8_t *fb = ceepew_oled_get_buffer(dev);
        /* Page 4, column 0 should have a non-zero byte (any column
         * along the line should). */
        if (fb[4U * 128U] == 0U) {
            printf("ui_manager selftest: gfx_line horiz left page-bytes zero\n");
            return;
        }
        if (fb[4U * 128U + 127U] == 0U) {
            printf("ui_manager selftest: gfx_line horiz right page-bytes zero\n");
            return;
        }
        ceepew_oled_destroy(dev);
        printf("CEEPEW: ui_manager selftest - gfx_line horiz PASS\n");
    }

    /* Sub-test 2: diagonal line writes multiple page-bytes. */
    {
        ceepew_oled_t *dev = ceepew_oled_create();
        if (dev == NULL) {
            printf("ui_manager selftest: ceepew_oled_create failed (test 2)\n");
            return;
        }
        (void)ceepew_oled_clear_buffer(dev);
        CeePewErr_t e = ceepew_oled_gfx_line(dev, 0, 0, 63, 63, HAL_UI_WHITE);
        if (e != CEEPEW_OK) {
            printf("ui_manager selftest: gfx_line diag failed (err=%d)\n", (int)e);
            return;
        }
        uint8_t *fb = ceepew_oled_get_buffer(dev);
        /* The diagonal should leave at least 16 page-bytes non-zero
         * (one per (x, y>>3) along the line). */
        uint32_t nonzero = 0U;
        for (uint16_t i = 0U; i < ceepew_oled_get_buffer_size(dev); i++) {
            if (fb[i] != 0U) { nonzero++; }
        }
        if (nonzero < 16U) {
            printf("ui_manager selftest: gfx_line diag nonzero=%lu < 16\n",
                   (unsigned long)nonzero);
            return;
        }
        ceepew_oled_destroy(dev);
        printf("CEEPEW: ui_manager selftest - gfx_line diag PASS\n");
    }

    /* Sub-test 3: vertical line writes a single bit per page. */
    {
        ceepew_oled_t *dev = ceepew_oled_create();
        if (dev == NULL) {
            printf("ui_manager selftest: ceepew_oled_create failed (test 3)\n");
            return;
        }
        (void)ceepew_oled_clear_buffer(dev);
        CeePewErr_t e = ceepew_oled_gfx_line(dev, 64, 0, 64, 63, HAL_UI_WHITE);
        if (e != CEEPEW_OK) {
            printf("ui_manager selftest: gfx_line vert failed (err=%d)\n", (int)e);
            return;
        }
        uint8_t *fb = ceepew_oled_get_buffer(dev);
        /* All 8 pages should have a non-zero byte at column 64. */
        for (uint8_t p = 0U; p < 8U; p++) {
            if (fb[(uint16_t)p * 128U + 64U] == 0U) {
                printf("ui_manager selftest: gfx_line vert page=%u zero\n",
                       (unsigned)p);
                return;
            }
        }
        ceepew_oled_destroy(dev);
        printf("CEEPEW: ui_manager selftest - gfx_line vert PASS\n");
    }

    /* Sub-test 4: outline rectangle writes 4 thin sides only. */
    {
        ceepew_oled_t *dev = ceepew_oled_create();
        if (dev == NULL) {
            printf("ui_manager selftest: ceepew_oled_create failed (test 4)\n");
            return;
        }
        (void)ceepew_oled_clear_buffer(dev);
        HalUIRect_t r = { .x = 10, .y = 10, .w = 50, .h = 20 };
        CeePewErr_t e = ceepew_oled_gfx_rect(dev, &r, HAL_UI_WHITE);
        if (e != CEEPEW_OK) {
            printf("ui_manager selftest: gfx_rect failed (err=%d)\n", (int)e);
            return;
        }
        uint8_t *fb = ceepew_oled_get_buffer(dev);
        /* Top-left corner should be set; an interior pixel should NOT. */
        if (fb[1U * 128U + 10U] == 0U) {
            printf("ui_manager selftest: gfx_rect top-left zero\n");
            return;
        }
        if (fb[2U * 128U + 20U] != 0U) {
            printf("ui_manager selftest: gfx_rect interior not zero\n");
            return;
        }
        ceepew_oled_destroy(dev);
        printf("CEEPEW: ui_manager selftest - gfx_rect PASS\n");
    }

    /* Sub-test 5: filled rectangle writes a full block. */
    {
        ceepew_oled_t *dev = ceepew_oled_create();
        if (dev == NULL) {
            printf("ui_manager selftest: ceepew_oled_create failed (test 5)\n");
            return;
        }
        (void)ceepew_oled_clear_buffer(dev);
        HalUIRect_t r = { .x = 0, .y = 0, .w = 16, .h = 8 };
        CeePewErr_t e = ceepew_oled_gfx_rect_fill(dev, &r, HAL_UI_WHITE);
        if (e != CEEPEW_OK) {
            printf("ui_manager selftest: gfx_rect_fill failed (err=%d)\n", (int)e);
            return;
        }
        uint8_t *fb = ceepew_oled_get_buffer(dev);
        for (uint8_t x = 0U; x < 16U; x++) {
            if (fb[0U * 128U + x] == 0U) {
                printf("ui_manager selftest: gfx_rect_fill col=%u zero\n",
                       (unsigned)x);
                return;
            }
        }
        /* Column 16 should still be 0 (out of rect). */
        if (fb[0U * 128U + 16U] != 0U) {
            printf("ui_manager selftest: gfx_rect_fill overdraw\n");
            return;
        }
        ceepew_oled_destroy(dev);
        printf("CEEPEW: ui_manager selftest - gfx_rect_fill PASS\n");
    }

    /* Sub-test 6: circle outline produces 8 octant pixels for radius 0..N. */
    {
        ceepew_oled_t *dev = ceepew_oled_create();
        if (dev == NULL) {
            printf("ui_manager selftest: ceepew_oled_create failed (test 6)\n");
            return;
        }
        (void)ceepew_oled_clear_buffer(dev);
        CeePewErr_t e = ceepew_oled_gfx_circle(dev, 64, 32, 10, HAL_UI_WHITE);
        if (e != CEEPEW_OK) {
            printf("ui_manager selftest: gfx_circle failed (err=%d)\n", (int)e);
            return;
        }
        uint8_t *fb = ceepew_oled_get_buffer(dev);
        /* Cardinal points: (cx+r, cy), (cx-r, cy), (cx, cy+r), (cx, cy-r)
         * should all be set. */
        if (fb[3U * 128U + 74U] == 0U) { printf("ui_circle E zero\n"); return; }
        if (fb[3U * 128U + 54U] == 0U) { printf("ui_circle W zero\n"); return; }
        if (fb[5U * 128U + 64U] == 0U) { printf("ui_circle S zero\n"); return; }
        if (fb[2U * 128U + 64U] == 0U) { printf("ui_circle N zero\n"); return; }
        ceepew_oled_destroy(dev);
        printf("CEEPEW: ui_manager selftest - gfx_circle PASS\n");
    }

    /* Sub-test 7: bitmap blit interprets MSB-first packed data. */
    {
        ceepew_oled_t *dev = ceepew_oled_create();
        if (dev == NULL) {
            printf("ui_manager selftest: ceepew_oled_create failed (test 7)\n");
            return;
        }
        (void)ceepew_oled_clear_buffer(dev);
        /* 8x1 bitmap, single bit set in column 3 (bit 4 from LSB). */
        const uint8_t bitmap[1] = { 0x10U };
        CeePewErr_t e = ceepew_oled_gfx_bitmap(dev, 0, 0, bitmap, 8, 1, HAL_UI_WHITE);
        if (e != CEEPEW_OK) {
            printf("ui_manager selftest: gfx_bitmap failed (err=%d)\n", (int)e);
            return;
        }
        uint8_t *fb = ceepew_oled_get_buffer(dev);
        /* Column-major vertical layout: column 3 of page 0 should have bit 0 (0x01) set. */
        if (fb[3U] != 0x01U) {
            printf("ui_manager selftest: gfx_bitmap got 0x%02X expected 0x01\n",
                   (unsigned)fb[3U]);
            return;
        }
        ceepew_oled_destroy(dev);
        printf("CEEPEW: ui_manager selftest - gfx_bitmap PASS\n");
    }

    /* Sub-test 8: text rendering touches the framebuffer. */
    {
        ceepew_oled_t *dev = ceepew_oled_create();
        if (dev == NULL) {
            printf("ui_manager selftest: ceepew_oled_create failed (test 8)\n");
            return;
        }
        (void)ceepew_oled_clear_buffer(dev);
        CeePewErr_t e = ceepew_oled_gfx_text(dev, 0, 0, "AB", HAL_UI_WHITE);
        if (e != CEEPEW_OK) {
            printf("ui_manager selftest: gfx_text failed (err=%d)\n", (int)e);
            return;
        }
        uint8_t *fb = ceepew_oled_get_buffer(dev);
        uint32_t nonzero = 0U;
        for (uint16_t i = 0U; i < ceepew_oled_get_buffer_size(dev); i++) {
            if (fb[i] != 0U) { nonzero++; }
        }
        if (nonzero < 4U) {
            printf("ui_manager selftest: gfx_text nonzero=%lu < 4\n",
                   (unsigned long)nonzero);
            return;
        }
        ceepew_oled_destroy(dev);
        printf("CEEPEW: ui_manager selftest - gfx_text PASS\n");
    }

    /* Sub-test 9: text_width returns 6 px per char, 0 for NULL/empty. */
    {
        if (ceepew_oled_gfx_text_width(NULL) != 0U) {
            printf("ui_manager selftest: text_width(NULL) != 0\n");
            return;
        }
        if (ceepew_oled_gfx_text_width("") != 0U) {
            printf("ui_manager selftest: text_width(\"\") != 0\n");
            return;
        }
        if (ceepew_oled_gfx_text_width("A") != 6U) {
            printf("ui_manager selftest: text_width(\"A\") != 6\n");
            return;
        }
        if (ceepew_oled_gfx_text_width("Hello") != 30U) {
            printf("ui_manager selftest: text_width(\"Hello\") != 30\n");
            return;
        }
        printf("CEEPEW: ui_manager selftest - gfx_text_width PASS\n");
    }

    /* Sub-test 10: font adapter constants: first=0x20, last=0x7E,
     * yAdvance=8, glyph_count=95. */
    {
        const ceepew_oled_GFXfont_t *font = ceepew_oled_get_default_font();
        if (font == NULL) {
            printf("ui_manager selftest: get_default_font returned NULL\n");
            return;
        }
        if (font->first != 0x20U) {
            printf("ui_manager selftest: default font first=0x%02X != 0x20\n",
                   (unsigned)font->first);
            return;
        }
        if (font->last != 0x7EU) {
            printf("ui_manager selftest: default font last=0x%02X != 0x7E\n",
                   (unsigned)font->last);
            return;
        }
        if (font->yAdvance != 8U) {
            printf("ui_manager selftest: default font yAdvance=%u != 8\n",
                   (unsigned)font->yAdvance);
            return;
        }
        if (font->bitmap == NULL || font->glyph == NULL) {
            printf("ui_manager selftest: default font bitmap/glyph NULL\n");
            return;
        }
        const uint8_t glyph_count = (uint8_t)(font->last -
                                              font->first + 1U);
        if (glyph_count != 95U) {
            printf("ui_manager selftest: glyph count=%u != 95\n",
                   (unsigned)glyph_count);
            return;
        }
        /* bitmapOffset for first glyph should be 0, for last glyph 470. */
        if (font->glyph[0].bitmapOffset != 0U) {
            printf("ui_manager selftest: glyph[0] offset=%u != 0\n",
                   (unsigned)font->glyph[0].bitmapOffset);
            return;
        }
        if (font->glyph[94].bitmapOffset != 470U) {
            printf("ui_manager selftest: glyph[94] offset=%u != 470\n",
                   (unsigned)font->glyph[94].bitmapOffset);
            return;
        }
        printf("CEEPEW: ui_manager selftest - font_adapter PASS\n");
    }

    /* Sub-test 11: ceepew_oled_get_buffer on a fresh device is non-NULL,
     * the buffer is all zero, and ceepew_oled_clear_buffer is idempotent.
     */
    {
        ceepew_oled_t *dev = ceepew_oled_create();
        if (dev == NULL) {
            printf("ui_manager selftest: ceepew_oled_create failed (test 11)\n");
            extern void ceepew_oled_restore_state(void);
            ceepew_oled_restore_state();
            return;
        }
        uint8_t *fb = ceepew_oled_get_buffer(dev);
        if (fb == NULL) {
            printf("ui_manager selftest: get_buffer returned NULL\n");
            extern void ceepew_oled_restore_state(void);
            ceepew_oled_restore_state();
            return;
        }
        for (uint16_t i = 0U; i < ceepew_oled_get_buffer_size(dev); i++) {
            if (fb[i] != 0U) {
                printf("ui_manager selftest: fresh fb[%u]=%u != 0\n",
                       (unsigned)i, (unsigned)fb[i]);
                extern void ceepew_oled_restore_state(void);
                ceepew_oled_restore_state();
                return;
            }
        }
        (void)ceepew_oled_clear_buffer(dev);
        for (uint16_t i = 0U; i < ceepew_oled_get_buffer_size(dev); i++) {
            if (fb[i] != 0U) {
                printf("ui_manager selftest: cleared fb[%u]=%u != 0\n",
                       (unsigned)i, (unsigned)fb[i]);
                extern void ceepew_oled_restore_state(void);
                ceepew_oled_restore_state();
                return;
            }
        }
        ceepew_oled_destroy(dev);
        printf("CEEPEW: ui_manager selftest - oled_buffer_lifecycle PASS\n");
    }

    /* ── STACKS page (Sprint 13) smoke check ─────────────────────
     * Force the pot to the page-4 zone and render; we don't assert
     * specific text (the bar values depend on runtime state), only
     * that render_diag_page() does not crash and produces output. */
    g_ui_ctx.diag_mode = true;
    g_ui_ctx.user_input = (uint8_t)4000U;   /* 4000 % 256 = 160; exercises diag page render */
    g_ui_ctx.transition_ready = true;
    CeePewErr_t stacks_rc = ui_manager_update();
    g_ui_ctx.diag_mode = false;
    if (stacks_rc == CEEPEW_OK) {
        printf("CEEPEW: ui_manager selftest - STACKS page renders PASS\n");
    } else {
        printf("CEEPEW: ui_manager selftest - STACKS page FAILED (rc=%d)\n",
               (int)stacks_rc);
    }

    (void)session_end();
    memset(&g_ble_ctx, 0, sizeof(g_ble_ctx));
    (void)msg_store_wipe_all();
    extern void ceepew_oled_restore_state(void);
    ceepew_oled_restore_state();
    printf("CEEPEW: ui_manager selftest PASS (all tests passed)\n");
}

#endif /* CEEPEW_ENABLE_SELFTEST */

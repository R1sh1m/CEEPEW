/* main/integration_test_pairing_ui.c
 *
 * Dedicated integration coverage for pairing UI convergence, layout safety,
 * and symmetric key-derivation behavior.
 */

#include "layout.h"
#include "session_fsm.h"
#include "ui_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "CEE-PEW-PAIRING-UI";

static const uint8_t DEVICE_A_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
static const uint8_t DEVICE_B_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
static const uint8_t SESSION_CODE[32] = {
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x30, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x30, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46
};

static bool pairing_ui_check(bool ok, const char *label)
{
    if (ok) {
        ESP_LOGI(TAG, "[PASS] %s", label);
        return true;
    }

    ESP_LOGE(TAG, "[FAIL] %s", label);
    return false;
}

static bool pairing_ui_run_session(const uint8_t self_mac[6],
                                   const uint8_t peer_mac[6],
                                   uint8_t key_out[16],
                                   uint8_t commitment_out[CEEPEW_COMMITMENT_BYTES])
{
    CEEPEW_ASSERT(self_mac != NULL, false);
    CEEPEW_ASSERT(peer_mac != NULL, false);

    if (!pairing_ui_check(session_phase1_init(self_mac) == CEEPEW_OK, "session_phase1_init")) {
        return false;
    }
    if (!pairing_ui_check(session_phase1_accept_peer(peer_mac) == CEEPEW_OK, "session_phase1_accept_peer")) {
        return false;
    }
    
    bool is_initiator = (memcmp(self_mac, peer_mac, 6) < 0);
    if (!pairing_ui_check(session_set_role(is_initiator) == CEEPEW_OK, "session_set_role")) {
        return false;
    }

    if (!pairing_ui_check(session_phase2_initiate(SESSION_CODE) == CEEPEW_OK, "session_phase2_initiate")) {
        return false;
    }
    if (!pairing_ui_check(session_phase2_derive_key() == CEEPEW_OK, "session_phase2_derive_key")) {
        return false;
    }
    if (!pairing_ui_check(session_get_session_key(key_out) == CEEPEW_OK, "session_get_session_key")) {
        return false;
    }
    if (!pairing_ui_check(session_get_commitment(commitment_out) == CEEPEW_OK, "session_get_commitment")) {
        return false;
    }
    if (!pairing_ui_check(session_get_phase() == 3U, "phase reached active")) {
        return false;
    }
    
    uint64_t expected_nonce = is_initiator ? 0ULL : 1ULL;
    if (!pairing_ui_check(session_get_nonce_counter() == expected_nonce, "nonce starts at expected value")) {
        return false;
    }
    return true;
}

static bool pairing_ui_validate_layout(void)
{
    const UIState_t states[] = {
        UI_STATE_PAIRING,
        UI_STATE_PAIRING_SUCCESS,
        UI_STATE_KEYDER,
    };

    for (uint8_t i = 0U; i < (uint8_t)(sizeof(states) / sizeof(states[0])); i++) {
        CeePewErr_t err = layout_validate_state_entry(states[i]);
        if (!pairing_ui_check(err == CEEPEW_OK, layout_state_name(states[i]))) {
            return false;
        }
    }
    return true;
}

static bool pairing_ui_validate_transitions(void)
{
    ui_manager_reset_to_discovery();
    if (!pairing_ui_check(ui_manager_transition_to(UI_STATE_PAIRING) == CEEPEW_OK, "transition to pairing")) {
        return false;
    }
    g_ui_ctx.transition_ready = true;
    if (!pairing_ui_check(ui_manager_update() == CEEPEW_OK, "enter pairing")) {
        return false;
    }
    if (!pairing_ui_check(g_ui_ctx.current_state == UI_STATE_PAIRING, "pairing state active")) {
        return false;
    }
    if (!pairing_ui_check(ui_manager_draw() == CEEPEW_OK, "render pairing")) {
        return false;
    }

    if (!pairing_ui_check(ui_manager_transition_to(UI_STATE_PAIRING_SUCCESS) == CEEPEW_OK, "transition to ready")) {
        return false;
    }
    g_ui_ctx.transition_ready = true;
    if (!pairing_ui_check(ui_manager_update() == CEEPEW_OK, "enter ready")) {
        return false;
    }
    if (!pairing_ui_check(g_ui_ctx.current_state == UI_STATE_PAIRING_SUCCESS, "ready state active")) {
        return false;
    }
    if (!pairing_ui_check(ui_manager_draw() == CEEPEW_OK, "render ready")) {
        return false;
    }

    /* Let the built-in ready hold elapse, then verify the KEYDER transition. */
    g_ui_ctx.pairing_result_start_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    vTaskDelay(pdMS_TO_TICKS(1300U));

    if (!pairing_ui_check(ui_manager_update() == CEEPEW_OK, "arm keyder transition")) {
        return false;
    }
    if (!pairing_ui_check(g_ui_ctx.next_state == UI_STATE_KEYDER, "keyder next state queued")) {
        return false;
    }
    if (!pairing_ui_check(g_ui_ctx.transition_ready, "keyder transition ready")) {
        return false;
    }
    if (!pairing_ui_check(ui_manager_update() == CEEPEW_OK, "enter keyder")) {
        return false;
    }
    if (!pairing_ui_check(g_ui_ctx.current_state == UI_STATE_KEYDER, "keyder state active")) {
        return false;
    }

    for (uint8_t frame = 0U; frame < 8U; frame++) {
        if (!pairing_ui_check(ui_manager_draw() == CEEPEW_OK, "render keyder")) {
            return false;
        }
        if (!pairing_ui_check(ui_manager_update() == CEEPEW_OK, "advance keyder")) {
            return false;
        }
    }

    return true;
}

bool test_pairing_ui_coverage(void)
{
    ESP_LOGI(TAG, "=== Pairing UI convergence coverage ===");

    if (!pairing_ui_validate_layout()) {
        return false;
    }

    if (!pairing_ui_validate_transitions()) {
        return false;
    }

    uint8_t key_ab[16];
    uint8_t commit_ab[CEEPEW_COMMITMENT_BYTES];
    uint8_t key_ba[16];
    uint8_t commit_ba[CEEPEW_COMMITMENT_BYTES];

    if (!pairing_ui_check(session_end() == CEEPEW_OK, "session_end before symmetry run")) {
        return false;
    }
    if (!pairing_ui_run_session(DEVICE_A_MAC, DEVICE_B_MAC, key_ab, commit_ab)) {
        return false;
    }

    if (!pairing_ui_check(session_end() == CEEPEW_OK, "session_end between symmetry runs")) {
        return false;
    }
    if (!pairing_ui_run_session(DEVICE_B_MAC, DEVICE_A_MAC, key_ba, commit_ba)) {
        return false;
    }

    if (!pairing_ui_check(memcmp(key_ab, key_ba, sizeof(key_ab)) == 0, "symmetric session key")) {
        return false;
    }
    if (!pairing_ui_check(memcmp(commit_ab, commit_ba, sizeof(commit_ab)) == 0, "symmetric commitment")) {
        return false;
    }

    if (!pairing_ui_check(session_end() == CEEPEW_OK, "session_end cleanup")) {
        return false;
    }

    ESP_LOGI(TAG, "Pairing UI convergence coverage PASS");
    return true;
}

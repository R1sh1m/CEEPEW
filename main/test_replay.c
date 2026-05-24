/* main/test_replay.c
 * Unit tests for WireGuard-style replay bitmap (transport_replay)
 */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <esp_log.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "TEST-REPLAY";

/* Forward declarations from transport_replay.c */
CeePewErr_t transport_replay_check(uint64_t msg_id, uint32_t timestamp, bool *is_replay);
void transport_replay_reset(void);

void test_replay_window(void){
    ESP_LOGI(TAG, "=== Test: Replay window behavior ===");

    uint32_t passed = 0U, failed = 0U;
    bool is_replay = false;

    transport_replay_reset();

    /* First packet should be accepted */
    CeePewErr_t err = transport_replay_check(1ULL, 0U, &is_replay);
    if (err == CEEPEW_OK && !is_replay) { ESP_LOGI(TAG, "PASS: first packet accepted"); passed++; } else { ESP_LOGE(TAG, "FAIL: first packet rejected"); failed++; }

    /* Next packet with higher seq accepted */
    err = transport_replay_check(2ULL, 0U, &is_replay);
    if (err == CEEPEW_OK && !is_replay) { ESP_LOGI(TAG, "PASS: next packet accepted"); passed++; } else { ESP_LOGE(TAG, "FAIL: next packet rejected"); failed++; }

    /* Duplicate packet should be detected as replay */
    err = transport_replay_check(2ULL, 0U, &is_replay);
    if (err == CEEPEW_OK && is_replay) { ESP_LOGI(TAG, "PASS: duplicate detected"); passed++; } else { ESP_LOGE(TAG, "FAIL: duplicate not detected"); failed++; }

    /* Too-old packet (beyond 64-window) should be rejected */
    transport_replay_reset();
    err = transport_replay_check(1000ULL, 0U, &is_replay);
    if (!(err == CEEPEW_OK && !is_replay)) { ESP_LOGE(TAG, "FAIL: init packet failed"); failed++; }
    uint64_t too_old = 1000ULL - (CEEPEW_REPLAY_WINDOW_SIZE + 1ULL);
    err = transport_replay_check(too_old, 0U, &is_replay);
    if (err == CEEPEW_OK && is_replay) { ESP_LOGI(TAG, "PASS: too-old detected"); passed++; } else { ESP_LOGE(TAG, "FAIL: too-old not detected"); failed++; }

    ESP_LOGI(TAG, "Replay test summary: passed=%u failed=%u", passed, failed);
}

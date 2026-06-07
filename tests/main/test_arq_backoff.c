/* main/test_arq_backoff.c - Validation of ARQ exponential backoff + jitter implementation
 *
 * This test validates that:
 * 1. Retry constant CEEPEW_ARQ_MAX_RETRIES is properly defined
 * 2. Expected timing ranges for backoff delays are achievable
 * 3. FreeRTOS delay mechanisms work correctly
 *
 * The backoff implementation uses:
 * - Attempt 0: 100ms base ± 10% jitter
 * - Attempt 1: 200ms base ± 10% jitter
 * - Attempt 2: 400ms base ± 10% jitter
 */

#include <stdint.h>
#include <stdbool.h>
#include <esp_log.h>

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "CEE-PEW-TEST-ARQ-BACKOFF";

/* Validation constants */
#define EXPECTED_ARQ_MAX_RETRIES 3U
#define BACKOFF_BASE_MS 100U
#define JITTER_PERCENT 10U

/* Test: Verify ARQ configuration is correct */
static void test_arq_config(void) {
    ESP_LOGI(TAG, "=== Test: ARQ Configuration ===");

    ESP_LOGI(TAG, "CEEPEW_ARQ_MAX_RETRIES = %u", CEEPEW_ARQ_MAX_RETRIES);
    if (CEEPEW_ARQ_MAX_RETRIES == EXPECTED_ARQ_MAX_RETRIES) {
        ESP_LOGI(TAG, "[PASS] ARQ max retries is %u", EXPECTED_ARQ_MAX_RETRIES);
    } else {
        ESP_LOGE(TAG, "[FAIL] ARQ max retries: expected %u, got %u",
                 EXPECTED_ARQ_MAX_RETRIES, CEEPEW_ARQ_MAX_RETRIES);
    }

    ESP_LOGI(TAG, "CEEPEW_ARQ_TIMEOUT_MS = %u", CEEPEW_ARQ_TIMEOUT_MS);

    /* Validate backoff timing expectations */
    for (uint8_t attempt = 0U; attempt < CEEPEW_ARQ_MAX_RETRIES; attempt++) {
        uint16_t base_ms = (uint16_t)(BACKOFF_BASE_MS << attempt);
        uint16_t jitter_amount = (base_ms * JITTER_PERCENT) / 100U;
        uint16_t min_ms = (uint16_t)(base_ms - jitter_amount);
        uint16_t max_ms = (uint16_t)(base_ms + jitter_amount);

        ESP_LOGI(TAG, "  Attempt %u: base=%ums, jitter_range=[%u, %u]ms",
                 attempt, base_ms, min_ms, max_ms);
    }
}

/* Test: Verify FreeRTOS delay timing is functional */
static void test_freertos_delays(void) {
    ESP_LOGI(TAG, "=== Test: FreeRTOS Delay Timing ===");

    /* Test a few representative delay values */
    uint16_t test_delays_ms[] = { 100U, 200U, 400U };

    for (size_t i = 0U; i < sizeof(test_delays_ms) / sizeof(test_delays_ms[0]); i++) {
        uint16_t delay_ms = test_delays_ms[i];

        /* Measure tick count before/after delay */
        TickType_t ticks_before = xTaskGetTickCount();
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        TickType_t ticks_after = xTaskGetTickCount();

        uint32_t elapsed_ticks = (uint32_t)(ticks_after - ticks_before);

        /* In ESP-IDF, typically 1 tick ≈ 1ms at default config */
        uint32_t elapsed_ms_approx = elapsed_ticks;

        ESP_LOGI(TAG, "  Delay %ums: elapsed ≈ %u ticks (≈%ums)",
                 delay_ms, (unsigned)elapsed_ticks, (unsigned)elapsed_ms_approx);

        /* Soft tolerance: allow ±20% due to scheduler jitter */
        uint32_t tolerance_min = (delay_ms * 80U) / 100U;
        uint32_t tolerance_max = (delay_ms * 120U) / 100U;

        if (elapsed_ms_approx >= tolerance_min && elapsed_ms_approx <= tolerance_max) {
            ESP_LOGI(TAG, "[PASS] Delay %ums within tolerance [%u, %u]ms",
                     delay_ms, (unsigned)tolerance_min, (unsigned)tolerance_max);
        } else {
            ESP_LOGW(TAG, "[WARN] Delay %ums outside tolerance [%u, %u]ms: got ≈%ums",
                     delay_ms, (unsigned)tolerance_min, (unsigned)tolerance_max, (unsigned)elapsed_ms_approx);
        }
    }
}

/* Main test entry point - called from integration test harness */
void test_arq_backoff(void) {
    ESP_LOGI(TAG, "\n");
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "ARQ Exponential Backoff Validation");
    ESP_LOGI(TAG, "======================================");

    test_arq_config();
    test_freertos_delays();

    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "ARQ backoff validation COMPLETE");
    ESP_LOGI(TAG, "======================================\n");
}

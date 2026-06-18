/*
 * Integration test: ESP-NOW round-trip latency and reliability.
 * Requires two devices, flashed with CEEPEW_TEST_MODE defined.
 * Run via monitor_both.py and look for PASS/FAIL at the end.
 */

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "session_fsm.h"
#include "session_send.h"
#include "crypto_ctx.h"

static const char *TAG = "test_espnow_roundtrip";

#define ROUNDTRIP_COUNT  20U
#define ROUNDTRIP_TIMEOUT_MS  2000U

/* Forward declaration */
CeePewErr_t session_send_roundtrip(const uint8_t *payload, uint16_t len, uint32_t timeout_ms);

void test_espnow_roundtrip(void)
{
    uint32_t success = 0;
    uint32_t fail    = 0;
    uint32_t total_latency_ms = 0;

    ESP_LOGI(TAG, "Starting ESP-NOW roundtrip test (%u iterations)...", ROUNDTRIP_COUNT);

    for (uint32_t i = 0U; i < ROUNDTRIP_COUNT; i++) {
        uint8_t payload[8];
        uint32_t seq = i;
        memcpy(payload, &seq, 4);
        memcpy(payload + 4, &seq, 4);   /* simple echo payload */

        uint32_t t_start = (uint32_t)(esp_timer_get_time() / 1000ULL);

        CeePewErr_t err = session_send_roundtrip(payload, sizeof(payload), ROUNDTRIP_TIMEOUT_MS);
        uint32_t latency = (uint32_t)((esp_timer_get_time() / 1000ULL) - t_start);

        if (err == CEEPEW_OK) {
            success++;
            total_latency_ms += latency;
            ESP_LOGI(TAG, "roundtrip %u PASS latency=%ums", i, latency);
        } else {
            fail++;
            ESP_LOGW(TAG, "roundtrip %u FAIL err=%d after %ums", i, (int)err, latency);
        }

        vTaskDelay(pdMS_TO_TICKS(100));   /* inter-packet gap */
    }

    ESP_LOGI(TAG, "ROUNDTRIP TEST: %u/%u success, avg_latency=%ums",
        success, ROUNDTRIP_COUNT,
        success > 0 ? total_latency_ms / success : 0);

    if (success == ROUNDTRIP_COUNT) {
        ESP_LOGI(TAG, "=== PASS ===");
    } else {
        ESP_LOGE(TAG, "=== FAIL === (%u lost)", fail);
    }
}

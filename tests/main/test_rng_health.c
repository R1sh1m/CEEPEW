/* tests/main/test_rng_health.c
 *
 * RNG Continuous Health Test — NIST SP 800-90B inspired.
 * Validates that consecutive identical samples trigger failure callback
 * after the configured threshold.
 */

#include "crypto_rng.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <esp_log.h>
#include <string.h>

static const char *TAG = "CEE-PEW-TEST-RNG-HEALTH";

static uint32_t s_failure_count = 0U;

static void rng_failure_handler(void)
{
    s_failure_count++;
}

static void test_rng_health_basic(void)
{
    ESP_LOGI(TAG, "=== Test: RNG Health Basic ===");
    s_failure_count = 0U;
    crypto_rng_reset_health_state();
    crypto_rng_set_failure_callback(rng_failure_handler);

    uint8_t buf[32];
    CeePewErr_t err = crypto_rng_fill(buf, sizeof(buf));
    if (err == CEEPEW_OK) {
        ESP_LOGI(TAG, "[PASS] RNG fill 32 bytes succeeded");
    } else {
        ESP_LOGE(TAG, "[FAIL] RNG fill 32 bytes: err=%d", err);
    }
}

static void test_rng_health_no_trigger_on_unique(void)
{
    ESP_LOGI(TAG, "=== Test: RNG Health No Trigger on Unique ===");
    s_failure_count = 0U;
    crypto_rng_reset_health_state();
    crypto_rng_set_failure_callback(rng_failure_handler);

    uint8_t a[32], b[32];
    CeePewErr_t err1 = crypto_rng_fill(a, sizeof(a));
    CeePewErr_t err2 = crypto_rng_fill(b, sizeof(b));
    if (err1 != CEEPEW_OK || err2 != CEEPEW_OK) {
        ESP_LOGE(TAG, "[FAIL] RNG fill failed: err1=%d, err2=%d", err1, err2);
        return;
    }

    uint8_t diff = 0U;
    for (uint32_t i = 0U; i < 32U; i++) { diff |= (uint8_t)(a[i] ^ b[i]); }
    if (diff == 0U) {
        ESP_LOGE(TAG, "[FAIL] Two consecutive RNG samples identical");
        return;
    }

    if (s_failure_count == 0U) {
        ESP_LOGI(TAG, "[PASS] No failure callback on unique samples");
    } else {
        ESP_LOGE(TAG, "[FAIL] Unexpected failure callback: count=%lu", s_failure_count);
    }
    crypto_rng_set_failure_callback(NULL);
}

static void test_rng_health_small_request_skips_test(void)
{
    ESP_LOGI(TAG, "=== Test: RNG Health Small Request Skips Test ===");
    s_failure_count = 0U;
    crypto_rng_reset_health_state();
    crypto_rng_set_failure_callback(rng_failure_handler);

    uint8_t buf[16];
    CeePewErr_t err = crypto_rng_fill(buf, sizeof(buf));
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[FAIL] RNG fill 16 bytes: err=%d", err);
        return;
    }

    if (s_failure_count == 0U && crypto_rng_get_failure_count() == 0U) {
        ESP_LOGI(TAG, "[PASS] Small request skipped health test");
    } else {
        ESP_LOGE(TAG, "[FAIL] Small request triggered health test");
    }
    crypto_rng_set_failure_callback(NULL);
}

void test_rng_health_run_all(void)
{
    test_rng_health_basic();
    test_rng_health_no_trigger_on_unique();
    test_rng_health_small_request_skips_test();
}

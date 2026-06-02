/* main/test_power.c - Integration test for power/wakeup handlers */

#include <stdint.h>
#include <stdbool.h>
#include <esp_log.h>

#include "ceepew_assert.h"
#include "integration_test_e2e.h"
#include "hal_pins.h"
#include "hal_power.h"

static const char *TAG = "CEE-PEW-TEST-POWER";

void test_power(void){
    ESP_LOGI(TAG, "=== Test: Power wakeup reason ===");

    CeePewErr_t err = hal_power_init();
    /* Reuse test utilities in integration_test_e2e.c by logging pass/fail here. */
    if (err == CEEPEW_OK) {
        ESP_LOGI(TAG, "[PASS] hal_power_init");
    } else {
        ESP_LOGE(TAG, "[FAIL] hal_power_init: %d", (int)err);
    }

    hal_power_wakeup_reason_t r = hal_power_get_wakeup_reason();
    if (r >= HAL_POWER_WAKEUP_UNDEFINED && r <= HAL_POWER_WAKEUP_UNKNOWN){
        ESP_LOGI(TAG, "[PASS] hal_power_get_wakeup_reason in range: %d", (int)r);
    } else {
        ESP_LOGE(TAG, "[FAIL] hal_power_get_wakeup_reason out of range: %d", (int)r);
    }
}

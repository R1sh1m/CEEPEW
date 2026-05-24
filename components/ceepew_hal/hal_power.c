/* components/ceepew_hal/hal_power.c */

#include "hal_pins.h"
#include "ceepew_assert.h"
#include "hal_power.h"

#include <stdint.h>
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/gpio.h"

/* Forward declarations for Phase 4 session lifecycle (called by task_session.c on TTL/nonce exhaustion) */
extern CeePewErr_t session_wipe(void);
extern void ui_manager_reset_to_discovery(void);

/* Initialize power/wakeup sources. Two CEEPEW_ASSERT checks per project rules. */
CeePewErr_t hal_power_init(void){
    CEEPEW_ASSERT(hal_pins_validate() == CEEPEW_OK, CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(GPIO_IS_VALID_GPIO(CEEPEW_PIN_BUTTON), CEEPEW_ERR_PINS);

    /* Phase 4: Brownout detection is hardware-managed in ESP32.
     * Recovery is via watchdog timeout or power restoration.
     * Brownout reset can be detected via esp_reset_reason() call. */

    /* Configure wake sources: external (button) and timer (default 1s). */
    esp_sleep_enable_ext0_wakeup((gpio_num_t)CEEPEW_PIN_BUTTON, CEEPEW_BUTTON_ACTIVE_LEVEL);
    esp_sleep_enable_timer_wakeup(1000ULL * 1000ULL); /* 1s in microseconds */

    return CEEPEW_OK;
}

/* Enter deep sleep for specified milliseconds. This function does not return.
 * Two asserts required for validation. */
void hal_power_enter_deepsleep(uint32_t ms){
    CEEPEW_ASSERT_VOID(ms > 0);
    CEEPEW_ASSERT_VOID(ms < 0xFFFFFFFFU / 1000U);

    /* Convert ms -> microseconds for esp_deep_sleep
     * esp_deep_sleep does not return. */
    esp_deep_sleep(((uint64_t)ms) * 1000ULL);
}

/* Return a normalized wakeup reason and record reset/wakeup sources.
 * Two CEEPEW_ASSERT checks included. */
hal_power_wakeup_reason_t hal_power_get_wakeup_reason(void){
    CEEPEW_ASSERT(hal_pins_validate() == CEEPEW_OK, CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(GPIO_IS_VALID_GPIO(CEEPEW_PIN_BUTTON), CEEPEW_ERR_PINS);

    /* Phase 4: Check for brownout reset reason */
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason == ESP_RST_BROWNOUT) {
        CEEPEW_LOG("HAL_POWER", "Detected brownout reset; session was lost");
    }

    uint32_t causes = esp_sleep_get_wakeup_causes();

    if (causes & (1u << ESP_SLEEP_WAKEUP_TIMER)) {
        return HAL_POWER_WAKEUP_TIMER;
    }
    if (causes & (1u << ESP_SLEEP_WAKEUP_EXT0)) {
        return HAL_POWER_WAKEUP_EXT0;
    }
    if (causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        return HAL_POWER_WAKEUP_GPIO;
    }

    if (causes == 0u) {
        return HAL_POWER_WAKEUP_UNDEFINED;
    }

    return HAL_POWER_WAKEUP_UNKNOWN;
}

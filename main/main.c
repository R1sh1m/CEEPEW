/* main/main.c - CEE-PEW Entry Point with Integration Tests */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "integration_test_e2e.h"
#include "../components/hal/hal_gpio.h"
#include "../components/ceepew_hal/hal_adc.h"
#include "../components/hal/hal_rng.h"
#include "../components/hal/hal_pins.h"
#include "../components/ceepew_hal/hal_oled.h"
#include "../components/ceepew_hal/hal_ui.h"
#include "../components/ceepew_hal/ui_manager.h"
#include "../components/ceepew_hal/task_arch.h"

static const char *TAG = "CEE-PEW-MAIN";

void app_main(void){
    ESP_LOGI(TAG, "=== CEE-PEW Firmware Startup ===");

    CeePewErr_t err = hal_gpio_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_gpio_init failed: %d", (int)err);
        return;
    }

    err = hal_pins_validate();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_pins_validate failed: %d", (int)err);
        return;
    }

    err = hal_adc_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_adc_init failed: %d", (int)err);
        return;
    }

    err = hal_rng_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_rng_init failed: %d", (int)err);
        return;
    }

    err = hal_oled_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_oled_init failed: %d", (int)err);
        return;
    }

    err = hal_ui_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_ui_init failed: %d", (int)err);
        return;
    }

    /* Initialize UI manager state machine before starting UI task */
    err = ui_manager_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "ui_manager_init failed: %d", (int)err);
        return;
    }

    err = task_arch_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "task_arch_init failed: %d", (int)err);
        return;
    }

    /* Print chip info */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip: %s (Rev %d)", CONFIG_IDF_TARGET, chip_info.revision);
    ESP_LOGI(TAG, "Cores: %d", chip_info.cores);
    ESP_LOGI(TAG, "Features: 0x%x", (unsigned)chip_info.features);

    /* Run integration tests */
    ESP_LOGI(TAG, "Starting integration tests...");
    integration_tests_run_all();

    ESP_LOGI(TAG, "=== CEE-PEW Ready ===");

    while (1){
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

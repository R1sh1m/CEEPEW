/* main/main.c - CEE-PEW Entry Point with Dual Task Launch */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"            /* ← required for esp_bt_controller_mem_release */
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "session_memory.h"
#include "ceepew_pipeline.h"
#include "task_ui.h"
#include "task_session.h"
#include "session_fsm.h"
#include "hal_radio.h"
#include "hal_gpio.h"
#include "hal_adc.h"
#include "hal_rng.h"
#include "crypto_rng.h"
#include "hal_pins.h"
#include "hal_oled.h"
#include "hal_ui.h"
#include "ui_manager.h"
#include "task_arch.h"
#include "esp_wifi.h"
#include "transport_ble.h"

static const char *TAG = "CEE-PEW-MAIN";

void app_main(void){
    ESP_LOGI(TAG, "=== CEE-PEW Firmware Startup ===");

    /* Initialize NVS (required by BLE stack before bring-up) */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition degraded; erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %d", (int)nvs_err);
        esp_restart();
    }

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

    /* RNG health audit: detect trivial HW RNG failures early and reboot */
    err = crypto_rng_health_check();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "RNG health check failed (%d) — rebooting", (int)err);
        esp_restart();
    }

    /* ── BT CONTROLLER MEMORY RELEASE ──────────────────────────────────────── */
    /* Must be called before esp_bt_controller_init() AND before WiFi starts.
     * This call carves out the correct memory layout for the BT controller,
     * including coexistence buffers shared with WiFi. If called after
     * esp_wifi_start() it returns ESP_ERR_INVALID_STATE and the controller may
     * be left with a misaligned memory map that breaks BLE scanning under
     * ESP-NOW. CEE-PEW uses BLE only, so release Classic BT memory here.
     */
    esp_err_t bt_mem_err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (bt_mem_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_bt_controller_mem_release: %d (%s) — continuing",
                 (int)bt_mem_err, esp_err_to_name(bt_mem_err));
    } else {
        ESP_LOGI(TAG, "Classic BT memory released to heap — BLE-only mode");
    }

    err = hal_radio_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_radio_init failed: %d", (int)err);
        return;
    }

    uint8_t local_mac[6] = {0};
    esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, local_mac);
    if (mac_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_mac failed: %d", (int)mac_err);
        return;
    }
    ESP_LOGI(TAG, "Local STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             local_mac[0], local_mac[1], local_mac[2],
             local_mac[3], local_mac[4], local_mac[5]);

    err = session_phase1_init(local_mac);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "session_phase1_init failed: %d", (int)err);
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

    /* CRITICAL: Initialize BLE stack before UI task (discover/pairing) */
    err = transport_ble_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "transport_ble_init failed: %d", (int)err);
        esp_restart();  /* Cannot proceed without BLE */
    }

    /* BLE advertising/scan start is driven from BLE GATT/GAP events to avoid race conditions */

    /* CRITICAL: Initialize region allocator before any pipeline or session use */
    err = region_init(&g_region);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "region_init failed: %d", (int)err);
        return;
    }

    /* CRITICAL: Initialize pipeline before session can deinit it */
    err = ceepew_pipeline_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "ceepew_pipeline_init failed: %d", (int)err);
        return;
    }

    /* Initialize task UI and session before task architecture */
    task_ui_init();
    task_session_init();

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

    /* Launch UI task on Core 0 */
    TaskHandle_t ui_handle = NULL;
    BaseType_t ui_result = xTaskCreatePinnedToCore(&task_ui_run, "UI", CEEPEW_CORE0_STACK_BYTES,
                                        NULL, CEEPEW_TASK_UI_PRIORITY, &ui_handle, 0);
    if (ui_result != pdPASS || ui_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create UI task (result=%d, handle=%p)", (int)ui_result, ui_handle);
        return;
    }
    ESP_LOGI(TAG, "UI task launched on Core 0");

    /* Launch session task on Core 1 */
    TaskHandle_t session_handle = NULL;
    BaseType_t session_result = xTaskCreatePinnedToCore(&task_session_run, "Session",
                                             CEEPEW_CORE1_STACK_BYTES, NULL,
                                             CEEPEW_TASK_SESSION_PRIORITY, &session_handle, 1);
    if (session_result != pdPASS || session_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create session task (result=%d, handle=%p)", (int)session_result, session_handle);
        return;
    }
    ESP_LOGI(TAG, "Session task launched on Core 1");

    ESP_LOGI(TAG, "=== CEE-PEW Ready ===");

    /* Tasks now run in background; app_main returns */
}

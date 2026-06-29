/* main/main.c — CEE-PEW entry point with dual task launch */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
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
#include "hal_i2c_scanner.h"
#include "hal_ui.h"
#include "ui_manager.h"
#include "task_arch.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "transport_ble.h"
#include "hal_temp.h"

static const char *TAG = "CEE-PEW-MAIN";

static void rng_failure_session_wipe(void) { session_request_wipe(); }

void app_main(void){
    ESP_LOGI(TAG, "=== CEE-PEW Firmware Startup ===");

    /* Suppress i2c.common false-positive GPIO reservation warnings */
    esp_log_level_set("i2c.common", ESP_LOG_ERROR);

    ESP_ERROR_CHECK(nvs_flash_init());

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

    /* Boot-time I2C scan when DIAG switch held LOW */
    {
        gpio_config_t io_cfg = {
            .pin_bit_mask = (1ULL << CEEPEW_PIN_DIAG_SWITCH),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        (void)gpio_config(&io_cfg);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(CEEPEW_PIN_DIAG_SWITCH) == CEEPEW_DIAG_SWITCH_ACTIVE) {
            ESP_LOGI(TAG, "DIAG switch active — running I2C scanner");
            (void)hal_i2c_scanner_scan_bus();
        } else {
            ESP_LOGI(TAG, "DIAG switch inactive — skipping I2C scanner");
        }
    }

    err = hal_adc_init();
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "hal_adc_init failed: %d", (int)err); return; }

    err = hal_temp_init();
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "hal_temp_init failed: %d", (int)err); return; }

    err = hal_rng_init();
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "hal_rng_init failed: %d", (int)err); return; }

    /* hal_ui_init wraps ceepew_oled. If the OLED is absent, it sets a
     * display-absent flag and the rest of the firmware runs headless. */
    err = hal_ui_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_ui_init failed: %d — continuing headless", (int)err);
    }

    /* Release Classic BT memory — BLE only */
    esp_err_t bt_mem_err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (bt_mem_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_bt_controller_mem_release: %d (%s) — continuing",
                 (int)bt_mem_err, esp_err_to_name(bt_mem_err));
    } else {
        ESP_LOGI(TAG, "Classic BT memory released to heap — BLE-only mode");
    }

    err = hal_radio_init();
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "hal_radio_init failed: %d", (int)err); return; }

    /* RNG health check with exponential backoff on transient failures */
    for (int attempt = 0; attempt < 3; attempt++) {
        err = crypto_rng_health_check();
        if (err == CEEPEW_OK) { break; }
        ESP_LOGW(TAG, "RNG health check failed (attempt %d/3): %d", attempt + 1, (int)err);
        if (attempt < 2) { vTaskDelay(pdMS_TO_TICKS(100 * (1 << attempt))); }
    }
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "RNG health check failed after retries — continuing with degraded entropy warning");
    }

    uint8_t local_mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(local_mac, ESP_MAC_BT);
    if (mac_err != ESP_OK) { ESP_LOGE(TAG, "esp_read_mac failed: %d", (int)mac_err); return; }
    ESP_LOGI(TAG, "Local BLE MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             local_mac[0], local_mac[1], local_mac[2],
             local_mac[3], local_mac[4], local_mac[5]);

    err = session_phase1_init(local_mac);

    uint8_t local_wifi_mac[6] = {0};
    esp_err_t wifi_mac_err = esp_read_mac(local_wifi_mac, ESP_MAC_WIFI_STA);
    if (wifi_mac_err == ESP_OK) { session_set_self_wifi_mac(local_wifi_mac); }
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "session_phase1_init failed: %d", (int)err); return; }

    crypto_rng_set_failure_callback(rng_failure_session_wipe);

    err = ui_manager_init();
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "ui_manager_init failed: %d", (int)err); return; }

    /* Initialize BLE before UI task. In dev mode, defer until after tests. */
#if !CONFIG_CEEPEW_DEVELOPMENT_MODE
    err = transport_ble_init();
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "transport_ble_init failed: %d", (int)err); esp_restart(); }
#endif

    err = region_init(&g_region);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "region_init failed: %d", (int)err); return; }

    err = ceepew_pipeline_init();
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "ceepew_pipeline_init failed: %d", (int)err); return; }

#if CONFIG_CEEPEW_DEVELOPMENT_MODE
    extern void test_pairing_handoff_run(void);
    test_pairing_handoff_run();

    extern void test_pairing_convergence_run(void);
    test_pairing_convergence_run();

    extern void integration_tests_run_all(void);
    integration_tests_run_all();

    extern void ui_manager_reset_to_discovery(void);
    ui_manager_reset_to_discovery();

    err = session_phase1_init(local_mac);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "session_phase1_init post-test failed: %d", (int)err); return; }

    /* Restore real WiFi MAC after tests overwrote it */
    wifi_mac_err = esp_read_mac(local_wifi_mac, ESP_MAC_WIFI_STA);
    if (wifi_mac_err == ESP_OK) { session_set_self_wifi_mac(local_wifi_mac); }

    err = transport_ble_init();
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "transport_ble_init failed: %d", (int)err); esp_restart(); }
#endif /* CONFIG_CEEPEW_DEVELOPMENT_MODE */

    task_ui_init();
    task_session_init();

    err = task_arch_init();
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "task_arch_init failed: %d", (int)err); return; }

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip: %s (Rev %d) Cores: %d", CONFIG_IDF_TARGET, chip_info.revision, chip_info.cores);

    TaskHandle_t ui_handle = NULL;
    BaseType_t ui_result = xTaskCreatePinnedToCore(&task_ui_run, "UI", CEEPEW_CORE0_STACK_BYTES,
                                        NULL, CEEPEW_TASK_UI_PRIORITY, &ui_handle, 0);
    if (ui_result != pdPASS || ui_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create UI task (result=%d, handle=%p)", (int)ui_result, ui_handle);
        return;
    }
    ESP_LOGI(TAG, "UI task launched on Core 0");

    TaskHandle_t session_handle = NULL;
    BaseType_t session_result = xTaskCreatePinnedToCore(&task_session_run, "Session",
                                             CEEPEW_CORE1_STACK_BYTES, NULL,
                                             CEEPEW_TASK_SESSION_PRIORITY, &session_handle, 1);
    if (session_result != pdPASS || session_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create session task (result=%d, handle=%p)", (int)session_result, session_handle);
        return;
    }
    ESP_LOGI(TAG, "Session task launched on Core 1");

#if CONFIG_CEEPEW_DEVELOPMENT_MODE
    extern void integration_test_pairing_e2e_run(void);
    integration_test_pairing_e2e_run();
#endif

    ESP_LOGI(TAG, "=== CEE-PEW Ready ===");
}

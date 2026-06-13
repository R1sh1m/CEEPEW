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
#include "hal_i2c_scanner.h"
#include "hal_ui.h"
#include "ui_manager.h"
#include "task_arch.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "transport_ble.h"

static const char *TAG = "CEE-PEW-MAIN";

static void rng_failure_session_wipe(void) { (void)session_wipe(); }

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

    /* Boot-time I2C diagnostic scan — only when DIAG switch held LOW at boot */
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
            ESP_LOGW(TAG, "DIAG switch active — running I2C scanner");
            (void)hal_i2c_scanner_scan_bus();
        } else {
            ESP_LOGI(TAG, "DIAG switch inactive — skipping I2C scanner");
        }
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

    /* Single init for both the panel and the UI layer. The previous
     * hal_oled_init() / hal_ui_init() pair is now collapsed into one call
     * because the UI layer wraps the in-house ceepew_oled panel handle. */
    err = hal_ui_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_ui_init failed: %d", (int)err);
#if CONFIG_CEEPEW_BUILD_TESTS
        ESP_LOGW(TAG, "Display init failed — continuing headless for diagnostic tests");
#else
        ESP_LOGE(TAG, "Display bring-up failed; restarting to avoid headless run");
        vTaskDelay(pdMS_TO_TICKS(2000U));
        esp_restart();
#endif
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

    /* RNG health audit: detect trivial HW RNG failures when RF is active and reboot */
    err = crypto_rng_health_check();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "RNG health check failed (%d) — rebooting", (int)err);
        esp_restart();
    }

    uint8_t local_mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(local_mac, ESP_MAC_BT);
    if (mac_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_read_mac failed: %d", (int)mac_err);
        return;
    }
    ESP_LOGI(TAG, "Local BLE MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             local_mac[0], local_mac[1], local_mac[2],
             local_mac[3], local_mac[4], local_mac[5]);

    err = session_phase1_init(local_mac);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "session_phase1_init failed: %d", (int)err);
        return;
    }

    /* Register RNG continuous health test failure callback.
     * 3 consecutive identical samples trigger session_wipe(). */
    crypto_rng_set_failure_callback(rng_failure_session_wipe);

    /* Initialize UI manager state machine before starting UI task */
    err = ui_manager_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "ui_manager_init failed: %d", (int)err);
        return;
    }

    /* CRITICAL: Initialize BLE stack before UI task (discover/pairing).
     * In test mode, defer BLE init until after diagnostics to avoid
     * scan-event flood saturating the 115200-baud serial link. */
#if !CONFIG_CEEPEW_BUILD_TESTS
    err = transport_ble_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "transport_ble_init failed: %d", (int)err);
        esp_restart();  /* Cannot proceed without BLE */
    }
#endif

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

    /* On-device pairing handoff regression test (runs once at boot) */
#if CONFIG_CEEPEW_BUILD_TESTS
    extern void test_pairing_handoff_run(void);
    test_pairing_handoff_run();

    /* On-device key-convergence + post-derive sync barrier regression
     * test. Exercises the fixes for the 4 critical pairing bugs. */
    extern void test_pairing_convergence_run(void);
    test_pairing_convergence_run();

    /* End-to-end integration test suite — prints === DIAGNOSTIC REPORT === */
    extern void integration_tests_run_all(void);
    integration_tests_run_all();

    /* Reset UI manager state to discovery so we don't start stuck in test outcomes */
    extern void ui_manager_reset_to_discovery(void);
    ui_manager_reset_to_discovery();

    /* Re-initialize session phase 1 for normal operation after tests */
    err = session_phase1_init(local_mac);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "session_phase1_init post-test failed: %d", (int)err);
        return;
    }

    /* Now initialize BLE for normal operation after tests complete */
    err = transport_ble_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "transport_ble_init failed: %d", (int)err);
        esp_restart();
    }
#endif /* CONFIG_CEEPEW_BUILD_TESTS */

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

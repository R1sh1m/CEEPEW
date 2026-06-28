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
#include "hal_temp.h"

static const char *TAG = "CEE-PEW-MAIN";

static void rng_failure_session_wipe(void) { session_request_wipe(); }

void app_main(void){
    ESP_LOGI(TAG, "=== CEE-PEW Firmware Startup ===");

    /* Suppress known false-positive warnings that create log noise.
     * i2c.common: GPIO 26/27 reservation warnings — the OLED works fine;
     * these fire because WiFi/BT reserve the RTC-domain GPIOs first. */
    esp_log_level_set("i2c.common", ESP_LOG_ERROR);

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
            ESP_LOGI(TAG, "DIAG switch active — running I2C scanner");
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

    err = hal_temp_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_temp_init failed: %d", (int)err);
        return;
    }

    err = hal_rng_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_rng_init failed: %d", (int)err);
        return;
    }

    /* Single init for both the panel and the UI layer. The previous
     * hal_oled_init() / hal_ui_init() pair is now collapsed into one call
     * because the UI layer wraps the in-house ceepew_oled panel handle.
     *
     * If the OLED does not ACK (missing or busy), hal_ui_init returns OK
     * with s_display_absent set; all subsequent hal_ui_* calls become
     * no-ops and log warnings. The rest of the firmware continues normally
     * so BLE, ESP-NOW, crypto, and the session FSM can be tested without
     * a working display panel. */
    err = hal_ui_init();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "hal_ui_init failed: %d — continuing headless", (int)err);
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

    /* RNG health audit: detect trivial HW RNG failures when RF is active.
     * Retry with exponential backoff instead of immediate reboot to handle
     * transient RNG anomalies (e.g., thermal noise). */
    for (int attempt = 0; attempt < 3; attempt++) {
        err = crypto_rng_health_check();
        if (err == CEEPEW_OK) {
            break;
        }
        ESP_LOGW(TAG, "RNG health check failed (attempt %d/3): %d", attempt + 1, (int)err);
        if (attempt < 2) {
            vTaskDelay(pdMS_TO_TICKS(100 * (1 << attempt)));
        }
    }
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "RNG health check failed after retries — continuing with degraded entropy warning");
        /* Do not reboot; mark entropy as degraded but allow operation.
         * Continuous RNG test will catch persistent failures at runtime. */
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

    uint8_t local_wifi_mac[6] = {0};
    esp_err_t wifi_mac_err = esp_read_mac(local_wifi_mac, ESP_MAC_WIFI_STA);
    if (wifi_mac_err == ESP_OK) {
        session_set_self_wifi_mac(local_wifi_mac);
    }
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
    /* Re-set self WiFi MAC — tests overwrote s_saved_self_wifi with test
     * MACs, so the real WiFi MAC must be restored for the GATT sign_pk
     * exchange and subsequent HW-gated identity check. */
    wifi_mac_err = esp_read_mac(local_wifi_mac, ESP_MAC_WIFI_STA);
    if (wifi_mac_err == ESP_OK) {
        session_set_self_wifi_mac(local_wifi_mac);
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

#if CONFIG_CEEPEW_BUILD_TESTS
    /* Two-device automated pairing + messaging integration test.
     * Creates a monitor task that tracks session FSM milestones and
     * exchanges a test message. See tests/main/integration_test_pairing_e2e.c */
    extern void integration_test_pairing_e2e_run(void);
    integration_test_pairing_e2e_run();
#endif

    ESP_LOGI(TAG, "=== CEE-PEW Ready ===");

    /* Tasks now run in background; app_main returns */
}

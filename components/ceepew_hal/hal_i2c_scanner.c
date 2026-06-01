/* components/ceepew_hal/hal_i2c_scanner.c
 *
 * I2C Bus Scanner — Boot-time Diagnostic Utility
 *
 * Design note: This scanner creates a temporary I2C bus on the primary pins
 * and probes all standard device addresses (0x03–0x77). It is intended to run
 * early in the boot sequence before the OLED and other I2C devices are
 * initialized, allowing developers to quickly identify I2C wiring issues and
 * address conflicts. Results are logged to the serial console using ESP_LOG.
 */

#include "hal_i2c_scanner.h"
#include "hal_pins.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "i2c_scanner";

CeePewErr_t hal_i2c_scanner_scan_bus(void)
{
    CEEPEW_ASSERT(GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SDA) &&
                  GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SCL), CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(I2C_SCAN_ADDR_MIN < I2C_SCAN_ADDR_MAX, CEEPEW_ERR_PARAM);

    ESP_LOGI(TAG, "Starting I2C bus scan on GPIO%d(SDA)/GPIO%d(SCL) at %lu Hz",
             (int)CEEPEW_PIN_I2C_SDA, (int)CEEPEW_PIN_I2C_SCL,
             (unsigned long)CEEPEW_I2C_FREQ_HZ);

    /* Create temporary I2C bus */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port              = CEEPEW_I2C_PORT,
        .sda_io_num            = CEEPEW_PIN_I2C_SDA,
        .scl_io_num            = CEEPEW_PIN_I2C_SCL,
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt     = 7U,
        .intr_priority         = 0,
        .trans_queue_depth     = 0U,  /* synchronous mode */
        .flags = {
            .enable_internal_pullup = 1U,
            .allow_pd               = 0U,
        },
    };

    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus");
        return CEEPEW_ERR_HW;
    }

    CEEPEW_ASSERT(bus != NULL, CEEPEW_ERR_HW);

    /* Allow bus to stabilize */
    vTaskDelay(pdMS_TO_TICKS(10U));

    uint8_t device_count = 0U;

    /* Scan all addresses in valid range */
    /* Loop bound: I2C_SCAN_ADDR_MAX - I2C_SCAN_ADDR_MIN + 1 = 0x75 = 117 iterations */
    for (uint8_t addr = I2C_SCAN_ADDR_MIN; addr <= I2C_SCAN_ADDR_MAX; addr++) {
        esp_err_t probe_result = i2c_master_probe(bus, addr, I2C_SCAN_PROBE_TIMEOUT_MS);
        if (probe_result == ESP_OK) {
            ESP_LOGI(TAG, "Device found at 0x%02X", (unsigned int)addr);
            device_count++;
        }
        /* Silently skip addresses that don't respond */
    }

    /* Clean up bus handle */
    esp_err_t delete_result = i2c_del_master_bus(bus);
    if (delete_result != ESP_OK) {
        ESP_LOGW(TAG, "Warning: i2c_del_master_bus returned 0x%X", (unsigned int)delete_result);
    }

    ESP_LOGI(TAG, "Scan complete. %u device(s) discovered.", (unsigned int)device_count);

    return CEEPEW_OK;
}

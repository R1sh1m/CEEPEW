/* components/ceepew_hal/hal_i2c_scanner.c
 *
 * I2C Bus Scanner — Boot-time Diagnostic Utility
 *
 * Design note: This scanner creates a temporary I2C bus on the specified pins
 * and probes all standard device addresses (0x03–0x77).
 */

#include "hal_i2c_scanner.h"
#include "hal_pins.h"
#include "ceepew_assert.h"

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"

#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "i2c_scanner";

static uint8_t get_board_tag(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        return mac[5];
    }
    return 0;
}

static uint32_t s_i2c_probe_attempts = 0;

static esp_err_t ceepew_scanner_i2c_probe(i2c_master_bus_handle_t bus, uint8_t addr, int timeout_ms)
{
    s_i2c_probe_attempts++;
    esp_err_t rc = i2c_master_probe(bus, addr, timeout_ms);
    ESP_LOGI(TAG, "[BOARD %02X] [OLED DIAG] [PROBE #%lu] Addr: 0x%02X, Timeout: %d ms, Return: %d (%s)",
             get_board_tag(), (unsigned long)s_i2c_probe_attempts, (unsigned int)addr, timeout_ms, (int)rc, esp_err_to_name(rc));
    return rc;
}

static void scan_pin_combination(gpio_num_t sda, gpio_num_t scl, const char *label,
                                  i2c_port_t port)
{
    ESP_LOGI(TAG, "[BOARD %02X] [%s] Starting I2C bus scan on GPIO%d(SDA)/GPIO%d(SCL) at %lu Hz (port %d)",
             get_board_tag(), label, (int)sda, (int)scl, (unsigned long)CEEPEW_I2C_FREQ_HZ, (int)port);

    gpio_reset_pin(sda);
    gpio_reset_pin(scl);

    const i2c_master_bus_config_t bus_cfg = {
        /* Use the caller-supplied port. Primary scan uses port 0 (CEEPEW_I2C_PORT).
         * Fallback scan uses port 1 to avoid contaminating port 0 — the OLED
         * driver (ceepew_oled.c) also uses port 0, and i2c_del_master_bus()
         * leaves the ESP32 I2C peripheral in a dirty state that prevents
         * i2c_master_transmit() from working until ~75 ms have elapsed.
         * Keeping the two ports separate eliminates the contamination entirely. */
        .i2c_port              = port,
        .sda_io_num            = sda,
        .scl_io_num            = scl,
        .clk_source            = I2C_CLK_SRC_APB,
        .glitch_ignore_cnt     = 0U,
        .intr_priority         = 0,
        .trans_queue_depth     = 0U,  /* synchronous mode */
        .flags = {
            .enable_internal_pullup = 1U,
            .allow_pd               = 0U,
        },
    };

    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "[BOARD %02X] [%s] Failed to create I2C bus", get_board_tag(), label);
        return;
    }

    /* Allow bus to stabilize */
    vTaskDelay(pdMS_TO_TICKS(75U));

    uint8_t device_count = 0U;
    bool found_3c = false;
    bool found_3d = false;

    for (uint8_t addr = I2C_SCAN_ADDR_MIN; addr <= I2C_SCAN_ADDR_MAX; addr++) {
        esp_err_t probe_result = ceepew_scanner_i2c_probe(bus, addr, I2C_SCAN_PROBE_TIMEOUT_MS);
        if (probe_result == ESP_OK) {
            ESP_LOGI(TAG, "[BOARD %02X] [%s] Device found at 0x%02X", get_board_tag(), label, (unsigned int)addr);
            if (addr == 0x3C) { found_3c = true; }
            if (addr == 0x3D) { found_3d = true; }
            device_count++;
        }
    }

    /* Clean up bus handle */
    esp_err_t delete_result = i2c_del_master_bus(bus);
    if (delete_result != ESP_OK) {
        ESP_LOGW(TAG, "[BOARD %02X] [%s] Warning: i2c_del_master_bus returned 0x%X", get_board_tag(), label, (unsigned int)delete_result);
    }

    /* Allow the peripheral hardware to complete its reset sequence after the bus
     * is deleted. Without this, a subsequent i2c_new_master_bus() on the same
     * port can succeed at the driver level but leave the peripheral unable to
     * clock out data bytes (symptoms: probe OK, transmit NACK). */
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    vTaskDelay(pdMS_TO_TICKS(75U));

    ESP_LOGI(TAG, "[BOARD %02X] [%s] Scan complete. %u device(s) discovered. 0x3C: %s, 0x3D: %s",
             get_board_tag(), label, (unsigned int)device_count,
             found_3c ? "FOUND" : "NOT FOUND",
             found_3d ? "FOUND" : "NOT FOUND");
}

CeePewErr_t hal_i2c_scanner_scan_bus(void)
{
    CEEPEW_ASSERT(GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SDA) &&
                  GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SCL), CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SDA_FALLBACK) &&
                  GPIO_IS_VALID_GPIO(CEEPEW_PIN_I2C_SCL_FALLBACK), CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(I2C_SCAN_ADDR_MIN < I2C_SCAN_ADDR_MAX, CEEPEW_ERR_PARAM);

    /* Primary scan: use port 0 (CEEPEW_I2C_PORT — the OLED's port).
     * Fallback scan: use port 1 to avoid a second create/destroy cycle on
     * port 0. This prevents the I2C0 peripheral from being left in a dirty
     * hardware state that would make hal_ui_init() fail with
     * ESP_ERR_INVALID_RESPONSE on every transmit attempt. */
    scan_pin_combination(CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL, "PRIMARY",
                         (i2c_port_t)1);
    scan_pin_combination(CEEPEW_PIN_I2C_SDA_FALLBACK, CEEPEW_PIN_I2C_SCL_FALLBACK, "FALLBACK",
                         (i2c_port_t)1);

    return CEEPEW_OK;
}

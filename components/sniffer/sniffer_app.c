/*
 * main/sniffer_app/sniffer_main.c
 *
 * CEE-PEW OTA Sniffer Firmware
 * Boots into promiscuous mode on channel 1, outputs ESP-NOW frames as hex via UART.
 * Press BOOT button (GPIO0) to stop capture and safe-exit.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sniffer.h"

static const char *TAG = "SNIFFER_APP";

/* BOOT button (GPIO0) - active low */
#define BOOT_BUTTON_GPIO    GPIO_NUM_0
#define DEBOUNCE_MS         50

static volatile bool s_stop_requested = false;

/* Button ISR - just sets flag */
static void IRAM_ATTR button_isr_handler(void *arg)
{
    (void)arg;
    s_stop_requested = true;
}

static void configure_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BOOT_BUTTON_GPIO, button_isr_handler, NULL));
}

void app_main(void)
{
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "  CEE-PEW OTA Sniffer Starting");
    ESP_LOGI(TAG, "  Channel: 1, ESP-NOW filter: ON");
    ESP_LOGI(TAG, "  Output: UART0 @ 921600 baud");
    ESP_LOGI(TAG, "  Press BOOT button to stop");
    ESP_LOGI(TAG, "=========================================");

    configure_button();

    sniffer_config_t config = SNIFFER_DEFAULT_CONFIG();
    config.channel = 1;
    config.filter_espnow_only = true;
    config.uart_port = UART_NUM_0;  /* Use UART0 (COM6) - mixed with logs/noise, filter in software */
    config.baud_rate = 921600;

    esp_err_t err = sniffer_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start sniffer: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Sniffer running. Capturing ESP-NOW frames...");

    /* Main loop - wait for button press */
    while (!s_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(100));

        /* Simple debounce */
        if (s_stop_requested) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                break;  /* Confirmed press */
            }
            s_stop_requested = false;  /* Bounce - ignore */
        }
    }

    ESP_LOGI(TAG, "Stop requested, shutting down...");

    err = sniffer_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error stopping sniffer: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Sniffer stopped. Safe to disconnect.");

    /* Blink LED pattern to indicate done (if LED available) */
    /* For now just loop */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
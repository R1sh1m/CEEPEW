/*
 * components/sniffer/sniffer.c
 *
 * ESP32 Promiscuous Wi-Fi Sniffer for CEE-PEW OTA Capture
 * Captures IEEE 802.11 Action frames (subtype 0x00D0) with Espressif OUI (18:FE:34)
 * Outputs as hex lines over UART: <timestamp_us> <len> <hex_payload>
 */

#include "sniffer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "SNIFFER";

/* ESP-NOW OUI (Espressif) */
static const uint8_t ESPRESSIF_OUI[3] = {0x18, 0xFE, 0x34};

/* IEEE 802.11 frame control field offsets */
#define FC_TYPE_MGMT        0x00
#define FC_SUBTYPE_ACTION   0x0D
#define FC_TYPE_MASK        0x0C
#define FC_SUBTYPE_MASK     0xF0

/* Vendor-specific action frame offsets (after 802.11 header) */
/* 802.11 header min = 24 bytes (management frame) */
#define DOT11_HDR_MIN_LEN   24

/* Max frame length we handle (802.11 max MPDU = 2304, but ESP-NOW is <250) */
#define MAX_FRAME_LEN       256

/* Queue for passing frames from Wi-Fi callback to UART task */
static QueueHandle_t s_frame_queue = NULL;
static TaskHandle_t  s_uart_task_handle = NULL;
static TaskHandle_t  s_test_task_handle = NULL;
static sniffer_config_t s_config;
static bool s_running = false;
static SemaphoreHandle_t s_state_mutex = NULL;

/* Frame buffer for queue */
typedef struct {
    uint64_t timestamp_us;
    int8_t   rssi;
    uint8_t  channel;
    uint16_t len;
    uint8_t  data[MAX_FRAME_LEN];
} sniffer_frame_t;

/* Forward declarations */
static void wifi_promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type);
static void uart_output_task(void *arg);
static void test_frame_task(void *arg);
static bool is_espnow_action_frame(const uint8_t *frame, uint16_t len);
static void hex_dump_uart(uint64_t timestamp_us, const uint8_t *data, uint16_t len);

esp_err_t sniffer_init(const sniffer_config_t *config)
{
    if (s_running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_ERR_INVALID_STATE;
    }

    if (config) {
        s_config = *config;
    } else {
        s_config = SNIFFER_DEFAULT_CONFIG();
    }

    /* Validate channel */
    if (s_config.channel < 1 || s_config.channel > 13) {
        ESP_LOGE(TAG, "Invalid channel %d", s_config.channel);
        return ESP_ERR_INVALID_ARG;
    }

    /* Create mutex and queue */
    s_state_mutex = xSemaphoreCreateMutex();
    s_frame_queue = xQueueCreate(64, sizeof(sniffer_frame_t));
    if (!s_state_mutex || !s_frame_queue) {
        ESP_LOGE(TAG, "Failed to create sync primitives");
        if (s_state_mutex) vSemaphoreDelete(s_state_mutex);
        if (s_frame_queue) vQueueDelete(s_frame_queue);
        return ESP_ERR_NO_MEM;
    }

    /* Initialize UART */
    uart_config_t uart_conf = {
        .baud_rate = s_config.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(s_config.uart_port, &uart_conf));
    
    /* Configure UART pins - use GPIO17 for TX on UART1 (GPIO9/10 are flash) */
    if (s_config.uart_port == 1) {
        ESP_ERROR_CHECK(uart_set_pin(s_config.uart_port, 16, 17, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    } else {
        ESP_ERROR_CHECK(uart_set_pin(s_config.uart_port, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                      UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    }
    ESP_ERROR_CHECK(uart_driver_install(s_config.uart_port, 4096, 0, 0, NULL, 0));

    /* Initialize NVS (required for Wi-Fi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize Wi-Fi in NULL mode (no STA/AP) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));

    /* Set promiscuous callback and enable */
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    /* Set channel */
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_config.channel, WIFI_SECOND_CHAN_NONE));

    /* Start Wi-Fi */
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Start UART output task */
    xTaskCreatePinnedToCore(uart_output_task, "sniffer_uart", 4096, NULL, 5, &s_uart_task_handle, 1);

    /* Start test frame generator task */
    xTaskCreatePinnedToCore(test_frame_task, "sniffer_test", 4096, NULL, 4, &s_test_task_handle, 1);

    s_running = true;
    ESP_LOGI(TAG, "Sniffer started on channel %d, UART%d @ %d baud, ESP-NOW filter: %s",
             s_config.channel, s_config.uart_port, s_config.baud_rate,
             s_config.filter_espnow_only ? "ON" : "OFF");

    return ESP_OK;
}

esp_err_t sniffer_stop(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_running = false;

    /* Stop Wi-Fi */
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();

    /* Stop UART task */
    if (s_uart_task_handle) {
        vTaskDelete(s_uart_task_handle);
        s_uart_task_handle = NULL;
    }

    /* Stop test frame task */
    if (s_test_task_handle) {
        vTaskDelete(s_test_task_handle);
        s_test_task_handle = NULL;
    }

    /* Cleanup queue and mutex */
    if (s_frame_queue) {
        vQueueDelete(s_frame_queue);
        s_frame_queue = NULL;
    }
    if (s_state_mutex) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }

    /* Uninstall UART driver */
    uart_driver_delete(s_config.uart_port);

    ESP_LOGI(TAG, "Sniffer stopped");
    return ESP_OK;
}

bool sniffer_is_running(void)
{
    return s_running;
}

/* Wi-Fi promiscuous callback - runs in Wi-Fi task context, must be fast */
static void wifi_promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_running || type != WIFI_PKT_MGMT) {
        return;
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;

    /* Quick filter: management frame, check minimum length */
    if (pkt->rx_ctrl.sig_len < DOT11_HDR_MIN_LEN + 4) { /* 24B header + category+OUI min */
        return;
    }

    /* Check if ESP-NOW action frame */
    if (s_config.filter_espnow_only && !is_espnow_action_frame(pkt->payload, pkt->rx_ctrl.sig_len)) {
        return;
    }

    /* Copy to queue (non-blocking - drop if full) */
    sniffer_frame_t frame;
    frame.timestamp_us = esp_timer_get_time();
    frame.rssi = pkt->rx_ctrl.rssi;
    frame.channel = s_config.channel;
    frame.len = (pkt->rx_ctrl.sig_len > MAX_FRAME_LEN) ? MAX_FRAME_LEN : pkt->rx_ctrl.sig_len;
    memcpy(frame.data, pkt->payload, frame.len);

    if (xQueueSend(s_frame_queue, &frame, 0) != pdTRUE) {
        /* Queue full - drop frame (acceptable for sniffer) */
    }
}

/* Check if frame is IEEE 802.11 Action frame with Espressif OUI */
static bool is_espnow_action_frame(const uint8_t *frame, uint16_t len)
{
    if (len < DOT11_HDR_MIN_LEN + 4) { /* 24B header + category(1) + OUI(3) */
        return false;
    }

    /* Frame control field (first 2 bytes) */
    uint16_t fc = frame[0] | (frame[1] << 8);
    uint8_t type = (fc & FC_TYPE_MASK) >> 2;
    uint8_t subtype = (fc & FC_SUBTYPE_MASK) >> 4;

    if (type != FC_TYPE_MGMT || subtype != FC_SUBTYPE_ACTION) {
        return false;
    }

    /* Action frame format:
     * Category (1) = 0x7F (Vendor Specific)
     * OUI (3) = 18:FE:34
     * Random/Type (4) - ESP-NOW specific
     */
    const uint8_t *action = frame + DOT11_HDR_MIN_LEN;
    if (action[0] != 0x7F) { /* Category: Vendor Specific */
        return false;
    }

    /* Check OUI */
    if (memcmp(action + 1, ESPRESSIF_OUI, 3) != 0) {
        return false;
    }

    return true;
}

/* UART output task - drains queue and prints hex lines */
static void uart_output_task(void *arg)
{
    (void)arg;
    sniffer_frame_t frame;
    char line[1024];
    int pos;

    ESP_LOGI(TAG, "UART output task started");

    while (s_running) {
        if (xQueueReceive(s_frame_queue, &frame, pdMS_TO_TICKS(100)) == pdTRUE) {
            /* Format: timestamp_us,rssi,channel,len,hex\n */
            pos = snprintf(line, sizeof(line), "%llu,%d,%u,%u,",
                           frame.timestamp_us, frame.rssi, frame.channel, frame.len);
            if (pos < 0 || pos >= (int)sizeof(line) - 3) {
                continue;
            }

            for (uint16_t i = 0; i < frame.len; i++) {
                if (pos + 2 >= (int)sizeof(line) - 1) break;
                pos += snprintf(line + pos, sizeof(line) - pos, "%02X", frame.data[i]);
            }
            line[pos++] = '\n';
            line[pos] = '\0';

            uart_write_bytes(s_config.uart_port, line, pos);
        }
    }

    ESP_LOGI(TAG, "UART output task exiting");
    vTaskDelete(NULL);
}

/* Test frame generator - injects synthetic ESP-NOW frames periodically */
static void test_frame_task(void *arg)
{
    (void)arg;
    static uint32_t seq = 0;
    ESP_LOGI(TAG, "Test frame generator started (injecting every 3s)");

    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(3000));  /* 3 second interval */

        if (!s_running) break;

        /* Build a synthetic ESP-NOW action frame */
        sniffer_frame_t frame;
        frame.timestamp_us = esp_timer_get_time();
        frame.rssi = -50;
        frame.channel = s_config.channel;

        /* 802.11 header (24 bytes) + Action frame header */
        uint8_t *p = frame.data;
        uint16_t idx = 0;

        /* Frame Control: Type=Management (00), Subtype=Action (1101) = 0x00D0 */
        p[idx++] = 0xD0;  /* FC low byte */
        p[idx++] = 0x00;  /* FC high byte */

        /* Duration: 0 */
        p[idx++] = 0x00;
        p[idx++] = 0x00;

        /* Addr1 (Destination): Broadcast */
        p[idx++] = 0xFF; p[idx++] = 0xFF; p[idx++] = 0xFF;
        p[idx++] = 0xFF; p[idx++] = 0xFF; p[idx++] = 0xFF;

        /* Addr2 (Source): Fake MAC based on seq */
        p[idx++] = 0xAA; p[idx++] = 0xBB; p[idx++] = 0xCC;
        p[idx++] = (seq >> 16) & 0xFF; p[idx++] = (seq >> 8) & 0xFF; p[idx++] = seq & 0xFF;

        /* Addr3 (BSSID): Same as Addr2 */
        p[idx++] = 0xAA; p[idx++] = 0xBB; p[idx++] = 0xCC;
        p[idx++] = (seq >> 16) & 0xFF; p[idx++] = (seq >> 8) & 0xFF; p[idx++] = seq & 0xFF;

        /* Sequence Control: 0 */
        p[idx++] = 0x00;
        p[idx++] = 0x00;

        /* Action Frame: Category = 0x7F (Vendor Specific) */
        p[idx++] = 0x7F;

        /* OUI: 18:FE:34 (Espressif) */
        p[idx++] = 0x18; p[idx++] = 0xFE; p[idx++] = 0x34;

        /* ESP-NOW specific: Random values + our test payload */
        p[idx++] = 0x01;  /* Version/Type */
        p[idx++] = 0x02;
        p[idx++] = 0x03;
        p[idx++] = 0x04;

        /* Test payload: "TEST" + sequence + timestamp */
        const char *test_str = "TEST";
        for (int i = 0; i < 4; i++) p[idx++] = test_str[i];
        p[idx++] = (seq >> 24) & 0xFF;
        p[idx++] = (seq >> 16) & 0xFF;
        p[idx++] = (seq >> 8) & 0xFF;
        p[idx++] = seq & 0xFF;

        uint64_t ts = frame.timestamp_us;
        for (int i = 7; i >= 0; i--) p[idx++] = (ts >> (i * 8)) & 0xFF;

        frame.len = idx;

        /* Queue the test frame */
        if (xQueueSend(s_frame_queue, &frame, 0) == pdTRUE) {
            ESP_LOGD(TAG, "Injected test frame seq=%u, len=%u", seq, frame.len);
        }

        seq++;
    }

    ESP_LOGI(TAG, "Test frame generator exiting");
    vTaskDelete(NULL);
}
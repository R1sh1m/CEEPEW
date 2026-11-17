/*
 * components/sniffer/sniffer.c
 *
 * ESP32 Promiscuous Wi-Fi Sniffer for CEE-PEW OTA Capture
 * Captures IEEE 802.11 Action frames (subtype 0x00D0) with Espressif OUI
 * (18:FE:34) Outputs as hex lines over UART: <timestamp_us> <len> <hex_payload>
 */

#include "sniffer.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "SNIFFER";

/* ESP-NOW OUI (Espressif) */
static const uint8_t ESPRESSIF_OUI[3] = {0x18, 0xFE, 0x34};

/* IEEE 802.11 frame control field offsets */
#define FC_TYPE_MGMT 0x00
#define FC_SUBTYPE_ACTION 0x0D
#define FC_TYPE_MASK 0x0C
#define FC_SUBTYPE_MASK 0xF0

/* Vendor-specific action frame offsets (after 802.11 header) */
/* 802.11 header min = 24 bytes (management frame) */
#define DOT11_HDR_MIN_LEN 24

/* Max frame length we handle (802.11 max MPDU = 2304, but ESP-NOW is <250) */
#define MAX_FRAME_LEN 256

/* Queue for passing frames from Wi-Fi callback to UART task */
static QueueHandle_t s_frame_queue = NULL;
static TaskHandle_t s_uart_task_handle = NULL;
static TaskHandle_t s_test_task_handle = NULL;
static sniffer_config_t s_config;
static bool s_running = false;
static SemaphoreHandle_t s_state_mutex = NULL;

/* Frame buffer for queue */
typedef struct {
  uint64_t timestamp_us;
  int8_t rssi;
  uint8_t channel;
  uint16_t len;
  uint8_t data[MAX_FRAME_LEN];
} sniffer_frame_t;

/* Forward declarations */
static void wifi_promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type);
static void uart_output_task(void *arg);
static void test_frame_task(void *arg);
static void uart_command_task(void *arg);
static bool is_espnow_action_frame(const uint8_t *frame, uint16_t len);
static esp_err_t inject_raw_frame(const uint8_t *data, uint16_t len);
static esp_err_t inject_bitflip_frame(const uint8_t *data, uint16_t len,
                                      int bit_index);
static esp_err_t inject_burst_frame(const uint8_t *data, uint16_t len,
                                    int start_bit, int num_bits);

esp_err_t sniffer_init(const sniffer_config_t *config) {
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
    if (s_state_mutex)
      vSemaphoreDelete(s_state_mutex);
    if (s_frame_queue)
      vQueueDelete(s_frame_queue);
    return ESP_ERR_NO_MEM;
  }

  /* Initialize UART for frame output and command input */
  uart_config_t uart_conf = {
      .baud_rate = s_config.baud_rate,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  if (s_config.uart_port == s_config.cmd_uart_port) {
    ESP_ERROR_CHECK(uart_param_config(s_config.uart_port, &uart_conf));
    if (s_config.uart_port == 1) {
      ESP_ERROR_CHECK(uart_set_pin(s_config.uart_port, 16, 17,
                                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    } else {
      ESP_ERROR_CHECK(uart_set_pin(s_config.uart_port, UART_PIN_NO_CHANGE,
                                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                   UART_PIN_NO_CHANGE));
    }
    ESP_ERROR_CHECK(
        uart_driver_install(s_config.uart_port, 4096, 4096, 0, NULL, 0));
  } else {
    // Output port
    ESP_ERROR_CHECK(uart_param_config(s_config.uart_port, &uart_conf));
    if (s_config.uart_port == 1) {
      ESP_ERROR_CHECK(uart_set_pin(s_config.uart_port, 16, 17,
                                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    } else {
      ESP_ERROR_CHECK(uart_set_pin(s_config.uart_port, UART_PIN_NO_CHANGE,
                                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                   UART_PIN_NO_CHANGE));
    }
    ESP_ERROR_CHECK(
        uart_driver_install(s_config.uart_port, 4096, 0, 0, NULL, 0));

    // Command port
    ESP_ERROR_CHECK(uart_param_config(s_config.cmd_uart_port, &uart_conf));
    ESP_ERROR_CHECK(uart_set_pin(s_config.cmd_uart_port, 4, 5,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(
        uart_driver_install(s_config.cmd_uart_port, 4096, 4096, 0, NULL, 0));
  }

  /* Initialize NVS (required for Wi-Fi) */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  /* Initialize Wi-Fi in STA mode so esp_wifi_80211_tx() works for injection */
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  /* Set promiscuous callback and enable */
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb));
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

  /* Set channel */
  ESP_ERROR_CHECK(
      esp_wifi_set_channel(s_config.channel, WIFI_SECOND_CHAN_NONE));

  /* Start Wi-Fi */
  ESP_ERROR_CHECK(esp_wifi_start());

  /* Start UART output task */
  xTaskCreatePinnedToCore(uart_output_task, "sniffer_uart", 4096, NULL, 5,
                          &s_uart_task_handle, 1);

  /* Start test frame generator task */
  xTaskCreatePinnedToCore(test_frame_task, "sniffer_test", 4096, NULL, 4,
                          &s_test_task_handle, 1);

  /* Start UART command task for host-controlled injection */
  xTaskCreatePinnedToCore(uart_command_task, "sniffer_cmd", 4096, NULL, 6, NULL,
                          1);

  s_running = true;
  ESP_LOGI(
      TAG,
      "Sniffer started on channel %d, UART%d @ %d baud, ESP-NOW filter: %s",
      s_config.channel, s_config.uart_port, s_config.baud_rate,
      s_config.filter_espnow_only ? "ON" : "OFF");

  return ESP_OK;
}

esp_err_t sniffer_stop(void) {
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

  /* Uninstall UART drivers */
  uart_driver_delete(s_config.uart_port);
  if (s_config.cmd_uart_port != s_config.uart_port) {
    uart_driver_delete(s_config.cmd_uart_port);
  }

  ESP_LOGI(TAG, "Sniffer stopped");
  return ESP_OK;
}

bool sniffer_is_running(void) { return s_running; }

/* Wi-Fi promiscuous callback - runs in Wi-Fi task context, must be fast */
static void wifi_promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (!s_running || type != WIFI_PKT_MGMT) {
    return;
  }

  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;

  /* Quick filter: management frame, check minimum length */
  if (pkt->rx_ctrl.sig_len <
      DOT11_HDR_MIN_LEN + 4) { /* 24B header + category+OUI min */
    return;
  }

  /* Check if ESP-NOW action frame */
  if (s_config.filter_espnow_only &&
      !is_espnow_action_frame(pkt->payload, pkt->rx_ctrl.sig_len)) {
    return;
  }

  /* Copy to queue (non-blocking - drop if full) */
  sniffer_frame_t frame;
  frame.timestamp_us = esp_timer_get_time();
  frame.rssi = pkt->rx_ctrl.rssi;
  frame.channel = s_config.channel;
  frame.len = (pkt->rx_ctrl.sig_len > MAX_FRAME_LEN) ? MAX_FRAME_LEN
                                                     : pkt->rx_ctrl.sig_len;
  memcpy(frame.data, pkt->payload, frame.len);

  if (xQueueSend(s_frame_queue, &frame, 0) != pdTRUE) {
    /* Queue full - drop frame (acceptable for sniffer) */
  }
}

/* Check if frame is IEEE 802.11 Action frame with Espressif OUI */
static bool is_espnow_action_frame(const uint8_t *frame, uint16_t len) {
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
static void uart_output_task(void *arg) {
  (void)arg;
  sniffer_frame_t frame;
  char line[1024];
  int pos;

  ESP_LOGI(TAG, "UART output task started");

  while (s_running) {
    if (xQueueReceive(s_frame_queue, &frame, pdMS_TO_TICKS(100)) == pdTRUE) {
      /* Format: timestamp_us,rssi,channel,len,hex\n */
      pos = snprintf(line, sizeof(line), "%llu,%d,%u,%u,", frame.timestamp_us,
                     frame.rssi, frame.channel, frame.len);
      if (pos < 0 || pos >= (int)sizeof(line) - 3) {
        continue;
      }

      for (uint16_t i = 0; i < frame.len; i++) {
        if (pos + 2 >= (int)sizeof(line) - 1)
          break;
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
static void test_frame_task(void *arg) {
  (void)arg;
  static uint32_t seq = 0;
  ESP_LOGI(TAG, "Test frame generator started (injecting every 3s)");

  while (s_running) {
    vTaskDelay(pdMS_TO_TICKS(3000)); /* 3 second interval */

    if (!s_running)
      break;

    /* Build a synthetic ESP-NOW action frame */
    sniffer_frame_t frame;
    frame.timestamp_us = esp_timer_get_time();
    frame.rssi = -50;
    frame.channel = s_config.channel;

    /* 802.11 header (24 bytes) + Action frame header */
    uint8_t *p = frame.data;
    uint16_t idx = 0;

    /* Frame Control: Type=Management (00), Subtype=Action (1101) = 0x00D0 */
    p[idx++] = 0xD0; /* FC low byte */
    p[idx++] = 0x00; /* FC high byte */

    /* Duration: 0 */
    p[idx++] = 0x00;
    p[idx++] = 0x00;

    /* Addr1 (Destination): Broadcast */
    p[idx++] = 0xFF;
    p[idx++] = 0xFF;
    p[idx++] = 0xFF;
    p[idx++] = 0xFF;
    p[idx++] = 0xFF;
    p[idx++] = 0xFF;

    /* Addr2 (Source): Fake MAC based on seq */
    p[idx++] = 0xAA;
    p[idx++] = 0xBB;
    p[idx++] = 0xCC;
    p[idx++] = (seq >> 16) & 0xFF;
    p[idx++] = (seq >> 8) & 0xFF;
    p[idx++] = seq & 0xFF;

    /* Addr3 (BSSID): Same as Addr2 */
    p[idx++] = 0xAA;
    p[idx++] = 0xBB;
    p[idx++] = 0xCC;
    p[idx++] = (seq >> 16) & 0xFF;
    p[idx++] = (seq >> 8) & 0xFF;
    p[idx++] = seq & 0xFF;

    /* Sequence Control: 0 */
    p[idx++] = 0x00;
    p[idx++] = 0x00;

    /* Action Frame: Category = 0x7F (Vendor Specific) */
    p[idx++] = 0x7F;

    /* OUI: 18:FE:34 (Espressif) */
    p[idx++] = 0x18;
    p[idx++] = 0xFE;
    p[idx++] = 0x34;

    /* ESP-NOW specific: Random values + our test payload */
    p[idx++] = 0x01; /* Version/Type */
    p[idx++] = 0x02;
    p[idx++] = 0x03;
    p[idx++] = 0x04;

    /* Test payload: "TEST" + sequence + timestamp */
    const char *test_str = "TEST";
    for (int i = 0; i < 4; i++)
      p[idx++] = test_str[i];
    p[idx++] = (seq >> 24) & 0xFF;
    p[idx++] = (seq >> 16) & 0xFF;
    p[idx++] = (seq >> 8) & 0xFF;
    p[idx++] = seq & 0xFF;

    uint64_t ts = frame.timestamp_us;
    for (int i = 7; i >= 0; i--)
      p[idx++] = (ts >> (i * 8)) & 0xFF;

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

/* ------------------------------------------------------------------------
 * Raw 802.11 frame injection via esp_wifi_80211_tx()
 * ------------------------------------------------------------------------ */
static esp_err_t inject_raw_frame(const uint8_t *data, uint16_t len) {
  if (!data || len == 0 || len > MAX_FRAME_LEN) {
    return ESP_ERR_INVALID_ARG;
  }

  /* esp_wifi_80211_tx requires Wi-Fi to be started; we are in NULL mode
   * which permits raw TX on the configured channel. */
  esp_err_t ret = esp_wifi_80211_tx(
      s_config.uart_port == 1 ? WIFI_IF_STA : WIFI_IF_STA, data, len, true);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_80211_tx failed: %d", ret);
    return ret;
  }
  ESP_LOGD(TAG, "Injected raw frame len=%u", len);
  return ESP_OK;
}

static esp_err_t inject_bitflip_frame(const uint8_t *data, uint16_t len,
                                      int bit_index) {
  if (!data || len == 0 || bit_index < 0 || bit_index >= (int)(len * 8)) {
    return ESP_ERR_INVALID_ARG;
  }

  /* Copy to a mutable buffer */
  static uint8_t buf[MAX_FRAME_LEN];
  if (len > MAX_FRAME_LEN) {
    return ESP_ERR_INVALID_ARG;
  }
  memcpy(buf, data, len);

  /* Flip the bit */
  int byte_idx = bit_index / 8;
  int bit_idx = bit_index % 8;
  buf[byte_idx] ^= (1 << bit_idx);

  ESP_LOGI(TAG, "Injecting bit-flip frame: bit=%d byte=%d mask=0x%02X",
           bit_index, byte_idx, (1 << bit_idx));
  return inject_raw_frame(buf, len);
}

static esp_err_t inject_burst_frame(const uint8_t *data, uint16_t len,
                                    int start_bit, int num_bits) {
  if (!data || len == 0 || start_bit < 0 || num_bits <= 0 ||
      start_bit + num_bits > (int)(len * 8)) {
    return ESP_ERR_INVALID_ARG;
  }

  static uint8_t buf[MAX_FRAME_LEN];
  if (len > MAX_FRAME_LEN) {
    return ESP_ERR_INVALID_ARG;
  }
  memcpy(buf, data, len);

  /* Flip each bit in the burst range */
  for (int i = 0; i < num_bits; i++) {
    int bit_index = start_bit + i;
    int byte_idx = bit_index / 8;
    int bit_idx = bit_index % 8;
    buf[byte_idx] ^= (1 << bit_idx);
  }

  ESP_LOGI(TAG, "Injecting burst frame: start_bit=%d num_bits=%d", start_bit,
           num_bits);
  return inject_raw_frame(buf, len);
}

/* Parse a hex string into a byte buffer. Returns number of bytes written. */
static int parse_hex(const char *hex_str, uint8_t *out, int max_len) {
  int len = (int)strlen(hex_str);
  if (len % 2 != 0)
    return -1;
  int byte_count = len / 2;
  if (byte_count > max_len)
    return -1;

  for (int i = 0; i < byte_count; i++) {
    unsigned int val;
    if (sscanf(hex_str + i * 2, "%02x", &val) != 1)
      return -1;
    out[i] = (uint8_t)val;
  }
  return byte_count;
}

/* UART command task - reads commands from UART and executes injection */
static void uart_command_task(void *arg) {
  (void)arg;
  static uint8_t cmd_buf[512];
  static uint8_t hex_buf[MAX_FRAME_LEN];

  ESP_LOGI(TAG, "UART command task started");

  while (s_running) {
    int len = uart_read_bytes(s_config.cmd_uart_port, cmd_buf,
                              sizeof(cmd_buf) - 1, pdMS_TO_TICKS(100));
    if (len <= 0)
      continue;

    cmd_buf[len] = '\0';

    /* Trim trailing whitespace/newline */
    while (len > 0 && (cmd_buf[len - 1] == '\n' || cmd_buf[len - 1] == '\r' ||
                       cmd_buf[len - 1] == ' ' || cmd_buf[len - 1] == '\t')) {
      cmd_buf[--len] = '\0';
    }

    if (len == 0)
      continue;

    /* Command: INJECT <hex_payload> */
    if (strncmp((char *)cmd_buf, "INJECT ", 7) == 0) {
      int payload_len = parse_hex((char *)cmd_buf + 7, hex_buf, MAX_FRAME_LEN);
      if (payload_len < 0) {
        ESP_LOGE(TAG, "INJECT: invalid hex payload");
        uart_write_bytes(s_config.uart_port, "INJECT ERROR\n", 13);
        continue;
      }
      esp_err_t ret = inject_raw_frame(hex_buf, (uint16_t)payload_len);
      if (ret == ESP_OK) {
        ESP_LOGI(TAG, "INJECT OK: %d bytes", payload_len);
        uart_write_bytes(s_config.uart_port, "INJECT OK\n", 10);
      } else {
        ESP_LOGE(TAG, "INJECT FAILED: %d", ret);
        uart_write_bytes(s_config.uart_port, "INJECT FAILED\n", 14);
      }
      continue;
    }

    /* Command: BITFLIP <hex_payload> <bit_index> */
    if (strncmp((char *)cmd_buf, "BITFLIP ", 8) == 0) {
      char *space = strchr((char *)cmd_buf + 8, ' ');
      if (!space) {
        ESP_LOGE(TAG, "BITFLIP: missing bit index");
        uart_write_bytes(s_config.uart_port, "BITFLIP ERROR\n", 14);
        continue;
      }
      *space = '\0';
      int bit_index = atoi(space + 1);
      int payload_len = parse_hex((char *)cmd_buf + 8, hex_buf, MAX_FRAME_LEN);
      if (payload_len < 0) {
        ESP_LOGE(TAG, "BITFLIP: invalid hex payload");
        uart_write_bytes(s_config.uart_port, "BITFLIP ERROR\n", 14);
        continue;
      }
      esp_err_t ret =
          inject_bitflip_frame(hex_buf, (uint16_t)payload_len, bit_index);
      if (ret == ESP_OK) {
        ESP_LOGI(TAG, "BITFLIP OK: bit=%d", bit_index);
        uart_write_bytes(s_config.uart_port, "BITFLIP OK\n", 11);
      } else {
        ESP_LOGE(TAG, "BITFLIP FAILED: %d", ret);
        uart_write_bytes(s_config.uart_port, "BITFLIP FAILED\n", 15);
      }
      continue;
    }

    /* Command: BURST <hex_payload> <start_bit> <num_bits> */
    if (strncmp((char *)cmd_buf, "BURST ", 6) == 0) {
      char *space1 = strchr((char *)cmd_buf + 6, ' ');
      if (!space1) {
        ESP_LOGE(TAG, "BURST: missing start_bit");
        uart_write_bytes(s_config.uart_port, "BURST ERROR\n", 12);
        continue;
      }
      *space1 = '\0';
      char *space2 = strchr(space1 + 1, ' ');
      if (!space2) {
        ESP_LOGE(TAG, "BURST: missing num_bits");
        uart_write_bytes(s_config.uart_port, "BURST ERROR\n", 12);
        continue;
      }
      *space2 = '\0';
      int start_bit = atoi(space1 + 1);
      int num_bits = atoi(space2 + 1);
      int payload_len = parse_hex((char *)cmd_buf + 6, hex_buf, MAX_FRAME_LEN);
      if (payload_len < 0) {
        ESP_LOGE(TAG, "BURST: invalid hex payload");
        uart_write_bytes(s_config.uart_port, "BURST ERROR\n", 12);
        continue;
      }
      esp_err_t ret = inject_burst_frame(hex_buf, (uint16_t)payload_len,
                                         start_bit, num_bits);
      if (ret == ESP_OK) {
        ESP_LOGI(TAG, "BURST OK: start=%d num=%d", start_bit, num_bits);
        uart_write_bytes(s_config.uart_port, "BURST OK\n", 9);
      } else {
        ESP_LOGE(TAG, "BURST FAILED: %d", ret);
        uart_write_bytes(s_config.uart_port, "BURST FAILED\n", 13);
      }
      continue;
    }

    /* Command: STATUS */
    if (strcmp((char *)cmd_buf, "STATUS") == 0) {
      ESP_LOGI(TAG, "STATUS: running=%d channel=%d filter=%d", s_running,
               s_config.channel, s_config.filter_espnow_only);
      uart_write_bytes(s_config.uart_port, "STATUS OK\n", 10);
      continue;
    }

    /* Command: STOP */
    if (strcmp((char *)cmd_buf, "STOP") == 0) {
      ESP_LOGI(TAG, "STOP command received");
      uart_write_bytes(s_config.uart_port, "EXITING\n", 8);
      sniffer_stop();
      continue;
    }

    ESP_LOGW(TAG, "Unknown command: %s", (char *)cmd_buf);
  }

  ESP_LOGI(TAG, "UART command task exiting");
  vTaskDelete(NULL);
}
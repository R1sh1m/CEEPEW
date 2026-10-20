/* components/transport/transport_capture.c
 *
 * On-node promiscuous OTA capture (opt-in Phase-3 debug aid).
 *
 * Compiled to real code ONLY when CONFIG_CEEPEW_CAPTURE is set (default n).
 * Normal builds see an empty translation unit: zero heap, zero footprint.
 *
 * NOTE (capture consumer requirement): the logger task streams one header
 * line plus hex rows per captured frame over the firmware console. Drain it
 * with the host serial monitor at 921600 baud or frames will be lost to UART
 * back-pressure. The global firmware console baud is intentionally NOT
 * changed here.
 *
 * Header ground truth (ESP-IDF 6.0.2, verified against
 * components/esp_wifi/include/local/esp_wifi_types_native.h):
 *   - wifi_promiscuous_pkt_t = { wifi_pkt_rx_ctrl_t rx_ctrl; uint8_t payload[0]; }
 *   - rx_ctrl fields used: rssi (signed 8-bit bitfield, dBm),
 *     channel (unsigned 4-bit), timestamp (unsigned 32-bit, us, wraps),
 *     sig_len (unsigned 12-bit, includes 4-byte FCS).
 *   - WIFI_PROMIS_FILTER_MASK_MGMT, wifi_promiscuous_pkt_type_t
 *     (WIFI_PKT_MGMT / CTRL / DATA / MISC), wifi_promiscuous_cb_t
 *     (void (*)(void *buf, wifi_promiscuous_pkt_type_t type)),
 *     esp_wifi_set_promiscuous_filter() /
 *     esp_wifi_set_promiscuous_rx_cb() / esp_wifi_set_promiscuous().
 *   - No IRAM_ATTR requirement is documented for the promiscuous callback;
 *     it is still marked IRAM_ATTR and kept ISR-light (copy only, no logs)
 *     as defensive practice.
 */

/* sdkconfig.h MUST come before the CONFIG_CEEPEW_CAPTURE guard below:
 * none of this TU's other headers provide it transitively, and without it
 * the guard is always false (verified: capture TU compiled empty even with
 * CONFIG_CEEPEW_CAPTURE=y). This is the ESP-IDF-idiomatic pattern. */
#include "sdkconfig.h"

#ifdef CONFIG_CEEPEW_CAPTURE

#include "transport_capture.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "capture";

/* ESP-NOW vendor action-frame OUI guard: payload[24] is the category
 * (0x7F = vendor-specific action), payload[25..27] is the Espressif OUI. */
#define TRANSPORT_CAPTURE_MIN_LEN 28U

/* One ring slot: radio metadata + truncated raw 802.11 bytes. */
typedef struct {
    uint32_t ts_us;
    int8_t rssi_dbm;
    uint8_t channel;
    uint16_t len;
    uint8_t buf[TRANSPORT_CAPTURE_MAX_BYTES];
} transport_capture_entry_t;

static transport_capture_entry_t s_ring[TRANSPORT_CAPTURE_RING_LEN];
static volatile uint8_t s_head = 0U;
static volatile uint8_t s_tail = 0U;
static volatile bool s_running = false;
static TaskHandle_t s_logger_task = NULL;

/* Static hex scratch for the logger task (never on the task stack). */
static char s_hex_row[100];

/* Static footprint guard: ring + scratch must stay well under budget. */
_Static_assert(sizeof(s_ring) <= 8192U, "capture ring exceeds 8 KB static budget");
_Static_assert(TRANSPORT_CAPTURE_RING_LEN == 16U, "capture ring depth changed");
_Static_assert(TRANSPORT_CAPTURE_MAX_BYTES == 300U, "capture payload cap changed");

/* ISR-light promiscuous callback: MGMT filter + OUI guard, copy, advance. */
static void IRAM_ATTR transport_capture_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) {
        return;
    }
    if (buf == NULL) {
        return;
    }
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    uint32_t sig_len = pkt->rx_ctrl.sig_len;
    if (sig_len < TRANSPORT_CAPTURE_MIN_LEN) {
        return;
    }
    const uint8_t *payload = pkt->payload;
    if (payload[24] != 0x7FU) {
        return;
    }
    if (payload[25] != 0x18U || payload[26] != 0xFEU || payload[27] != 0x34U) {
        return;
    }
    uint16_t copy_len = (sig_len > TRANSPORT_CAPTURE_MAX_BYTES)
                        ? TRANSPORT_CAPTURE_MAX_BYTES
                        : (uint16_t)sig_len;
    uint8_t idx = s_head;
    s_ring[idx].ts_us = pkt->rx_ctrl.timestamp;
    s_ring[idx].rssi_dbm = (int8_t)pkt->rx_ctrl.rssi;
    s_ring[idx].channel = (uint8_t)pkt->rx_ctrl.channel;
    s_ring[idx].len = copy_len;
    memcpy(s_ring[idx].buf, payload, copy_len);
    idx = (uint8_t)((idx + 1U) % TRANSPORT_CAPTURE_RING_LEN);
    if (idx == s_tail) {
        /* Ring full: drop oldest, never block the RX path. */
        s_tail = (uint8_t)((s_tail + 1U) % TRANSPORT_CAPTURE_RING_LEN);
    }
    s_head = idx;
}

/* Low-priority logger task: prints ts,rssi,ch,len,hex per ring entry.
 * Polls the ring; all formatting uses static buffers only. */
static void transport_capture_logger_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t head = s_head;
        while (s_tail != head) {
            uint8_t idx = s_tail;
            uint32_t ts_us = s_ring[idx].ts_us;
            int rssi_dbm = (int)s_ring[idx].rssi_dbm;
            unsigned channel = (unsigned)s_ring[idx].channel;
            unsigned len = (unsigned)s_ring[idx].len;
            ESP_LOGI(TAG, "cap ts=%lu rssi=%d ch=%u len=%u",
                     (unsigned long)ts_us, rssi_dbm, channel, len);
            for (uint16_t off = 0U; off < s_ring[idx].len;) {
                uint16_t row_len = (uint16_t)(s_ring[idx].len - off);
                if (row_len > 32U) {
                    row_len = 32U;
                }
                uint16_t pos = 0U;
                for (uint16_t i = 0U; i < row_len; i++) {
                    pos += (uint16_t)snprintf(s_hex_row + pos,
                                              sizeof(s_hex_row) - pos,
                                              "%02X ", s_ring[idx].buf[off + i]);
                    if (pos >= sizeof(s_hex_row) - 4U) {
                        break;
                    }
                }
                s_hex_row[pos] = '\0';
                ESP_LOGI(TAG, "  %s", s_hex_row);
                off = (uint16_t)(off + row_len);
            }
            s_tail = (uint8_t)((idx + 1U) % TRANSPORT_CAPTURE_RING_LEN);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

CeePewErr_t transport_capture_start(void)
{
    if (s_running) {
        return CEEPEW_OK;
    }
    wifi_promiscuous_filter_t filt;
    memset(&filt, 0, sizeof(filt));
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    if (esp_wifi_set_promiscuous_filter(&filt) != ESP_OK) {
        return CEEPEW_ERR_HW;
    }
    if (esp_wifi_set_promiscuous_rx_cb(transport_capture_cb) != ESP_OK) {
        return CEEPEW_ERR_HW;
    }
    /* Zero the ring BEFORE enabling the RX path: a frame arriving between
     * set_promiscuous(true) and the memset below would be wiped and the
     * head/tail reset would race the callback's advance. */
    memset(s_ring, 0, sizeof(s_ring));
    s_head = 0U;
    s_tail = 0U;
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        return CEEPEW_ERR_HW;
    }
    if (s_logger_task == NULL) {
        if (xTaskCreate(transport_capture_logger_task, "cap_log", 2048,
                        NULL, 1, &s_logger_task) != pdPASS) {
            (void)esp_wifi_set_promiscuous(false);
            return CEEPEW_ERR_HW;
        }
    }
    s_running = true;
    ESP_LOGI(TAG, "on-node capture started (MGMT filter, OUI 18:fe:34)");
    return CEEPEW_OK;
}

void transport_capture_stop(void)
{
    (void)esp_wifi_set_promiscuous(false);
    if (s_logger_task != NULL) {
        vTaskDelete(s_logger_task);
        s_logger_task = NULL;
    }
    s_running = false;
}

bool transport_capture_is_running(void)
{
    return s_running;
}

#endif /* CONFIG_CEEPEW_CAPTURE */

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

#include "transport_capture.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
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

/* Static footprint guard: ring must stay well under budget. */
_Static_assert(sizeof(s_ring) <= 8192U, "capture ring exceeds 8 KB static budget");
_Static_assert(TRANSPORT_CAPTURE_RING_LEN == 4U, "capture ring depth changed");
_Static_assert(TRANSPORT_CAPTURE_MAX_BYTES == 288U, "capture payload cap changed");

/* ISR-light promiscuous callback: MGMT filter + OUI guard, copy, advance. */
static void IRAM_ATTR transport_capture_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    /* All promiscuous packet types are delivered to this callback; the OUI
     * guard below selects ESP-NOW vendor action frames. */
    if (buf == NULL) {
        return;
    }
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    uint32_t sig_len = pkt->rx_ctrl.sig_len;
    if (sig_len < TRANSPORT_CAPTURE_MIN_LEN) {
        return;
    }
    const uint8_t *payload = pkt->payload;
    /* ESP-NOW vendor action-frame guard (original design, restored
     * 2026-10-19): payload[24] = category 0x7F, payload[25..27] = Espressif
     * OUI 18:FE:34. The "accept-all" variant flooded the 4-slot ring with
     * beacon/MISC noise, so the rare DUT frames were overwritten before the
     * logger could drain them (E1_A_plain_20261018 / E1_C_ceepew_20261018
     * splits: zero-to-few DUT rows despite ~87 peer frames received).
     * Offsets verified against captured DUT frames in Modes A/B/C. */
    if (payload[24] != 0x7FU || payload[25] != 0x18U ||
        payload[26] != 0xFEU || payload[27] != 0x34U) {
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

/* Low-priority logger task: prints one single-line CSV row per ring entry:
 *   ts_us,rssi_dbm,channel,len,HEX (contiguous %02X, no spaces/colons).
 * Matches tools/capture/serial_to_pcap.py LINE_RE and
 * tools/promisc_capture_helper.py RING_LINE_RE. Raw printf (no ESP_LOGI
 * tag prefix) so the strict RING_LINE_RE ^\d+, anchor holds. Polls the
 * ring; zero-heap, static ring only. */
static void transport_capture_logger_task(void *arg)
{
    (void)arg;
    /* The assembled row lives on THIS task's stack, not in .bss: a static
     * 768-byte row buffer pushed .dram0.bss past dram0_0_seg (link failed,
     * 448 bytes over, 2026-10-19). This task exists solely to format and
     * emit rows, and its stack was raised 1536 -> 4096 bytes to hold the
     * worst-case row (24 prefix + 2*288 hex + newline) plus stdio/VFS
     * frames. Capture semantics are unchanged. */
    char row[768];
    for (;;) {
        uint8_t head = s_head;
        while (s_tail != head) {
            uint8_t idx = s_tail;
            /* Build the whole row in `row` and emit it with ONE stdio call.
             * The previous per-byte printf loop released the console lock
             * between bytes, so other tasks' log lines interleaved mid-row
             * and corrupted the strict RING_LINE_RE format (observed as
             * ~30% loose-only rows in the E1_A/E1_C splits). */
            int n = snprintf(row, sizeof(row), "%lu,%d,%u,%u,",
                             (unsigned long)s_ring[idx].ts_us,
                             (int)s_ring[idx].rssi_dbm,
                             (unsigned)s_ring[idx].channel,
                             (unsigned)s_ring[idx].len);
            if (n < 0) {
                n = 0;
            }
            for (uint16_t i = 0U; i < s_ring[idx].len; i++) {
                if ((size_t)n >= sizeof(row) - 3U) {
                    break;
                }
                (void)snprintf(&row[n], sizeof(row) - (size_t)n, "%02X",
                               s_ring[idx].buf[i]);
                n += 2;
            }
            row[n++] = '\n';
            (void)fwrite(row, 1U, (size_t)n, stdout);
            (void)fflush(stdout);
            s_tail = (uint8_t)((idx + 1U) % TRANSPORT_CAPTURE_RING_LEN);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

CeePewErr_t transport_capture_start(void)
{
    if (s_running) {
        return CEEPEW_OK;
    }
    wifi_promiscuous_filter_t filt;
    memset(&filt, 0, sizeof(filt));
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
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
        /* 4096 bytes: 768-byte row staging buffer on this task's stack plus
         * stdio/VFS/UART frames (was 1536 before the single-write row fix). */
        if (xTaskCreate(transport_capture_logger_task, "cap_log", 4096,
                        NULL, 1, &s_logger_task) != pdPASS) {
            (void)esp_wifi_set_promiscuous(false);
            return CEEPEW_ERR_HW;
        }
    }
    s_running = true;
    ESP_LOGI(TAG, "on-node capture started (ESP-NOW OUI guard 7F:18FE34)");
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

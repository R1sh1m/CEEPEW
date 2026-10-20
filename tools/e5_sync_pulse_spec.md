# E5: Sync-Pulse Latency Measurement — Firmware Spec

## Objective
Measure **one-way over-the-air latency** with microsecond resolution, isolating:
- TX pipeline (submit → pulse → send-callback)
- OTA wire time
- RX pipeline (pulse-edge ISR → RX callback → accept)

This addresses the paper's acknowledged weakness: "the ≈13.9 ms gap ... is an untested hypothesis (FreeRTOS scheduling plus 115,200-baud UART logging)".

## Hardware
- **Wire**: GPIO14 (Board A, output) → GPIO16 (Board B, input)
- **Ground**: Shared via USB (both boards on same hub) or dedicated jumper
- **Distance**: 20 cm, fixed orientation
- **Channel**: 1 (hopping OFF for timing runs)

## Firmware Changes

### A. TX Side (Board A) — `main/task_session.c`

Add under `CONFIG_CEEPEW_BENCH_58B && CONFIG_CEEPEW_BENCH_TIMING`:

```c
// In session_send.c, stage_tx_encrypt or session_send_message:
static uint64_t s_bench_t_submit_us = 0;
static uint64_t s_bench_t_pulse_us = 0;

// Before esp_now_send():
s_bench_t_submit_us = esp_timer_get_time();
hal_gpio_write(CONFIG_CEEPEW_PIN_SYNC_PULSE_OUT, 1);  // Raise GPIO14
s_bench_t_pulse_us = esp_timer_get_time();
hal_gpio_write(CONFIG_CEEPEW_PIN_SYNC_PULSE_OUT, 0);  // Lower after ~1-2 µs

// In esp_now send callback:
static uint64_t s_bench_t_send_cb_us = 0;
s_bench_t_send_cb_us = esp_timer_get_time();
```

### B. RX Side (Board B) — `main/task_session.c` or `hal_gpio.c`

```c
// ISR for GPIO16 edge:
static uint64_t s_bench_t_pulse_edge_us = 0;
static void IRAM_ATTR bench_pulse_isr(void *arg) {
    s_bench_t_pulse_edge_us = esp_timer_get_time();
}

// In RX callback entry:
static uint64_t s_bench_t_rx_cb_us = 0;
s_bench_t_rx_cb_us = esp_timer_get_time();

// After accept (post-Ascon/box/edverify):
static uint64_t s_bench_t_accept_us = 0;
s_bench_t_accept_us = esp_timer_get_time();
```

### C. Storage & Dump

```c
// Static ring buffer (256 entries × 6 timestamps × 8 B = 12 KB):
typedef struct {
    uint64_t t_submit_us;
    uint64_t t_pulse_us;
    uint64_t t_send_cb_us;
    uint64_t t_pulse_edge_us;  // RX-only, TX leaves 0
    uint64_t t_rx_cb_us;       // RX-only
    uint64_t t_accept_us;      // RX-only
    uint32_t seq;
    uint8_t  role;  // 0=TX, 1=RX
} bench_timing_rec_t;

static bench_timing_rec_t s_bench_ring[256];
static uint8_t s_bench_head = 0;

// Dump at end of BENCH58 campaign:
for (uint8_t i = 0; i < s_bench_head; i++) {
    printf("BENCH_TIME seq=%lu role=%u submit=%llu pulse=%llu sendcb=%llu "
           "edge=%llu rxcb=%llu accept=%llu\n",
           (unsigned long)s_bench_ring[i].seq,
           (unsigned)s_bench_ring[i].role,
           (unsigned long long)s_bench_ring[i].t_submit_us,
           (unsigned long long)s_bench_ring[i].t_pulse_us,
           (unsigned long long)s_bench_ring[i].t_send_cb_us,
           (unsigned long long)s_bench_ring[i].t_pulse_edge_us,
           (unsigned long long)s_bench_ring[i].t_rx_cb_us,
           (unsigned long long)s_bench_ring[i].t_accept_us);
}
```

### D. Kconfig Options

```kconfig
config CEEPEW_BENCH_TIMING
    bool "Enable sync-pulse timing instrumentation"
    depends on CEEPEW_BENCH_58B
    help
        Record microsecond-resolution timestamps for OTA latency measurement.
        Requires GPIO wire between boards (CONFIG_CEEPEW_PIN_SYNC_PULSE_OUT →
        CONFIG_CEEPEW_PIN_SYNC_PULSE_IN).

config CEEPEW_PIN_SYNC_PULSE_OUT
    int "Sync pulse output GPIO (TX board)"
    default 14
    range 0 39

config CEEPEW_PIN_SYNC_PULSE_IN
    int "Sync pulse input GPIO (RX board)"
    default 16
    range 0 39

config CEEPEW_BENCH_TIMING_ROLE
    string "Timing role: 'tx' or 'rx'"
    default "tx"
    help
        TX board generates pulse before send; RX board captures edge via ISR.
        For round-trip measurement, flash both roles and run twice.
```

## Analysis Script

```python
# tools/bench_latency_report.py
# Input: monitor log with BENCH_TIME lines
# Output:
#   - TX pipeline: submit→pulse, pulse→sendcb (µs, N, median, IQR, p95)
#   - OTA wire: pulse→edge (one-way latency, µs)
#   - RX pipeline: edge→rxcallback, rxcallback→accept
#   - End-to-end: submit→accept (receiver-side goodput)
```

## Run Plan

| Run | TX Board | RX Board | Role A | Role B | N |
|-----|----------|----------|--------|--------|---|
| E5-1 | COM3 | COM5 | tx | rx | 80 |
| E5-2 | COM5 | COM3 | rx | tx | 80 (swap roles) |
| E5-3 | COM3 | COM5 | tx | rx | 80 (rate sweep: 1000/700/500/350 ms) |

## Blocking Issues
- [ ] DRAM overflow fix (build_cap_ceepew won't link)
- [ ] msgstore_unread_count include fix (task_ui.c)
- [ ] GPIO init: ensure GPIO14/16 not used elsewhere (check pin map)
- [ ] ISR safety: `esp_timer_get_time()` is ISR-safe? (yes, per ESP-IDF docs)

## Evidence Class
- **M-HW**: Hardware-timestamped, no-logging run (UART silent during timed region)
- **C**: Computed-from-hardware-log (existing BENCH58 runs with UART logging)

## Notes
- GPIO14 on ESP32 is **not** a strapping pin — safe for output
- GPIO16 on ESP32 is **not** a strapping pin — safe for input
- Series resistor (1kΩ) recommended but not required for 3.3V→3.3V
- Both boards share USB ground; dedicated ground jumper reduces noise

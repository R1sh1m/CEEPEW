# E5: Sync-Pulse Latency Measurement — Firmware Spec

> **Status:** implemented. Source of truth is
> `components/ceepew_hal/bench_timing.h` / `bench_timing.c`.
> This document describes the design that shipped. It replaces an earlier
> draft that specified a one-directional `GPIO14 → GPIO16` link with separate
> `tx`/`rx` firmware roles; that design was never implemented and is
> **methodologically invalid** — see *Why not a one-way link* below.

## Objective

Measure **one-way over-the-air latency** with microsecond resolution, isolating:

| Interval | Clock |
|---|---|
| TX pipeline: `submit → pulse` | transmitter |
| TX pipeline: `pulse → send_cb` | transmitter |
| RX pipeline: `edge → rx_cb` (one-way OTA) | receiver |
| RX pipeline: `rx_cb → accept` | receiver |

This addresses the paper's acknowledged weakness: the ≈13.9 ms gap between RF
airtime and measured send-callback latency is, absent this instrumentation, an
untested hypothesis (FreeRTOS scheduling plus UART logging).

## Hardware

- **Wire:** a single jumper between **GPIO14 on both boards** (`D14 → D14`).
  There is no output pin and no input pin; both boards drive *and* sense the
  same line.
- **Electrical mode:** `GPIO_MODE_INPUT_OUTPUT_OD` (open-drain) with the
  internal pull-up enabled on both ends. Assert = drive LOW; release = drive
  HIGH, which on an open-drain pin is high-Z, so the pull-up restores the line.
  Push-pull would short 3.3 V to GND when both boards drive at once.
- **Ground:** shared via USB (both boards on the same hub) or a dedicated
  jumper.
- **Distance:** 20 cm, fixed orientation.
- **Channel:** 1, channel hopping OFF for timing runs.

GPIO14 is not a strapping pin and is outside the application pin map
(OLED 26/27, pot 33, button 19, diag 5, RGB 15/18/23), so it carries no
protocol traffic and is free for instrumentation.

### Why not a one-way link

The two ESP32s run independent, un-synchronised `esp_timer` clocks. A one-way
`A_out → B_in` link still leaves the measurement undefined, because
`t_edge(B) − t_pulse(A)` subtracts two different time bases; the result absorbs
the entire power-on offset between the boards and is not a latency.

A shared line fixes this by substituting a physical event for clock
synchronisation. The falling edge is a single physical event observed by both
boards, so the interval that starts at the edge lies **entirely on one clock**:

```
A: submit ──► pulse(assert LOW) ═══════════ 802.11 frame ══════════► send_cb
                                    │
                                    └─ shared falling edge
                                                     │
B:                        edge ──────────────────────┴──► rx_cb ──► accept
                          └──────────── receiver clock ─────────────┘
```

`edge → rx_cb` is the valid one-way figure. `pulse → edge` is never computed.

## Firmware

Implemented in `components/ceepew_hal/bench_timing.{h,c}`; hooked from
`session_send.c` (TX) and `task_session.c` (RX). All entry points are
`static inline` no-ops when `CONFIG_CEEPEW_BENCH_TIMING` is off, so non-bench
builds are unaffected.

### A. API

```c
void     bench_timing_init(void);                            /* once, before the radio transmits */
uint64_t bench_timing_pulse(void);                           /* assert LOW ~5 µs, return t_assert */
void     bench_timing_record_tx(uint32_t seq, uint64_t t_submit_us);
void     bench_timing_record_send_cb(uint32_t seq);
void     bench_timing_record_edge(uint64_t t_edge_us);       /* inert — see below */
void     bench_timing_record_rx_cb(uint32_t seq, uint64_t t_rx_cb_us);
void     bench_timing_note_accept(uint32_t seq);
void     bench_timing_dump(void);                            /* emits BENCH_TIME_DUMP_COMPLETE */
```

`bench_timing_init()` must be called from `app_main()` **before**
`hal_radio_init()` can transmit. This is not optional: skipping it leaves
GPIO14 unconfigured, no pulse is ever emitted and no edge is ever captured,
and the campaign silently yields an all-zero timing dump. That failure has
already cost one run — see `data/raw/E5_sync_pulse_20261018/RUN_NOTES.md`.

### B. Edge capture and self-attribution

- The GPIO ISR (`IRAM_ATTR`, NEGEDGE) latches `esp_timer_get_time()` into
  `s_isr_edge_timestamp_us`. No logging, no allocation, no blocking.
- `bench_timing_record_edge()` is deliberately **inert**; the ISR timestamp is
  authoritative.
- Driving the shared line low latches an edge on the board's *own* input path
  too. `bench_timing_pulse()` clears the pending latch after release, and
  `bench_timing_record_rx_cb()` additionally rejects any edge within
  `BENCH_TIMING_SELF_EDGE_GUARD_US` (2000 µs) of the board's own last pulse.
  A rejected or absent edge is recorded as `edge=0` — "no peer pulse
  observed" — rather than as a fabricated interval. The analysis tool buckets
  those as a lost pulse.
- Records are streamed by `printf` as they complete (TX at `send_cb`, RX at
  `accept`); there is no ring buffer and nothing to flush. `bench_timing_dump()`
  only emits the end-of-campaign marker.

### C. Line format

```
BENCH_TIME seq=<n> role=<tx|rx> submit=<u64> pulse=<u64> sendcb=<u64> edge=<u64> rxcb=<u64> accept=<u64>
```

Fields not applicable to the record's role are written as `0`. All values are
`esp_timer_get_time()` microseconds on the emitting board's clock.

### D. Kconfig

```kconfig
config CEEPEW_BENCH_58B
    bool "Enable BENCH58 scripted frame campaign"
    default n

config CEEPEW_BENCH_TIMING
    bool "Enable sync-pulse timing instrumentation (E5)"
    default n
    depends on CEEPEW_BENCH_58B

config CEEPEW_PIN_SYNC
    int "Sync pulse GPIO (shared wire, both boards)"
    default 14
    range 0 39
    depends on CEEPEW_BENCH_TIMING
```

There are no `*_PULSE_OUT`, `*_PULSE_IN` or `*_TIMING_ROLE` options. Both
boards run the same firmware image; role is not compiled in.

## Analysis

`tools/bench_latency_report.py` consumes `BENCH_TIME` lines and emits
`latency.json` / `latency.md` into `data/derived/<run-id>/`. It reports:

- TX: `submit → pulse`, `pulse → send_cb`
- RX: `edge → rx_cb` (one-way OTA only when `edge != 0`), `rx_cb → accept`

**Hard rule:** no cross-device subtraction, ever. The tool never mixes
timestamps from two boards. Its `--self-test` proves this by feeding two
devices whose clock bases differ by 1e6 µs and asserting the reported intervals
are unchanged and all < 10 ms. The JSON carries an `interpretation` field
documenting the same-clock rule so a reviewer cannot mistake a cross-clock
difference for a measurement.

## Run Plan

Both boards are flashed with the same build and both pulse and sense on every
frame; statistics are then grouped by device and role.

| Run | Board A | Board B | Frames/board | Notes |
|-----|---------|---------|--------------|-------|
| E5-1 | COM3 | COM5 | 80 @ 5 s | baseline, hopping off |
| E5-2 | COM5 | COM3 | 80 @ 5 s | repeat, positions swapped |

## Failure Modes

| Symptom | Cause |
|---|---|
| All `BENCH_TIME` fields zero | `bench_timing_init()` never called |
| `edge=0` on every RX record | wire absent/broken, wrong pin, or peer not pulsing |
| `edge → rx_cb` implausibly small | self-edge not rejected; check the 2000 µs guard |
| No `BENCH_TIME` lines at all | `CONFIG_CEEPEW_BENCH_TIMING` off, or `BENCH_58B` off (it is a dependency) |

## Evidence Class

- **M-HW**: hardware-timestamped, no logging inside the timed region.
- **C**: computed from hardware logs (BENCH58 runs with UART logging active).

## Notes

- GPIO14 on ESP32 is **not** a strapping pin — safe for open-drain I/O.
- A 1 kΩ series resistor is recommended but not required for 3.3 V → 3.3 V.
- Both boards share USB ground; a dedicated ground jumper reduces noise.
- Leaving the jumper attached in a normal build is harmless: with
  `CONFIG_CEEPEW_BENCH_TIMING` off, GPIO14 is never configured.

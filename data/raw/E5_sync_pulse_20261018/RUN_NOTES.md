# E5 — Microsecond sync-pulse one-way latency — run notes

## What was measured, how, and N

Two ESP32 boards, 20 cm apart, sharing a **single jumper wire between GPIO14 of
both boards**. Both boards transmit *and* receive during a BENCH58 campaign (80
frames each, one every 5 s), so each board must pulse and sense on the same pin.
The line is configured `GPIO_MODE_INPUT_OUTPUT_OD` with the internal pull-up:
assert = drive LOW, release = drive HIGH (high-Z). Two push-pull outputs on one
wire would short 3.3 V to GND, so open-drain is mandatory here, and it also makes
simultaneous assertion harmless (wire-AND). The **falling** edge is timed, because
it is driver-dominated and sharp whereas the rising edge is pull-up RC-limited.

Timestamps are `esp_timer_get_time()` microseconds. **The two boards have
independent clocks**, so only same-clock intervals are reported and the analysis
tool performs no cross-board subtraction:
- **TX side:** `submit → pulse` (deciding to send → asserting the line) and
  `pulse → sendcb` (line asserted → ESP-NOW send callback).
- **RX side:** `edge → rxcb` — **the headline one-way figure** — and
  `rxcb → accept` (receive-side processing).
  The pulse crosses a 20 cm wire in nanoseconds and the frame is handed to the
  radio immediately after the pulse, so `edge → rxcb` captures radio TX +
  propagation + RX + delivery-to-callback entirely within the receiver's own clock.

N: **80 TX records per board** (seq 0..79, 0 gaps, both boards), **71 RX records**
(DEVICE_A) and **68 RX records** (DEVICE_B). 300 BENCH_TIME records total
(160 tx, 140 rx) recovered from a 9,710,434 B dual capture over 480 s.

## RESULT (median; IQR in parentheses)

| interval | DEVICE_A | DEVICE_B |
|---|---|---|
| TX `submit → pulse` | **365.524 ms** (357.340–371.738) | **370.274 ms** (365.632–375.772) |
| TX `pulse → sendcb` | **4.364 ms** (4.223–4.626) | **4.366 ms** (4.240–4.636) |
| RX `edge → rxcb` **(one-way)** | **26.620 ms** (20.141–37.552) | **27.214 ms** (19.806–33.982) |
| RX `rxcb → accept` | **727.284 ms** (715.593–737.457) | **725.586 ms** (717.143–734.664) |

Extremes: `edge → rxcb` min 6.696 ms (A) / 6.774 ms (B); `submit → pulse` min
346.658 ms (A) / 347.076 ms (B), max 395.325 ms (A) / 389.087 ms (B).

Interpretation, stated conservatively: `submit → pulse` is dominated by the
on-device crypto + ARQ path (~365 ms, consistent with the independently measured
Ed25519 sign cost of ~332 ms). `rxcb → accept` (~726 ms) is receive-side
processing, consistent with Ed25519 verify + Ascon decrypt. **The one-way radio
figure is `edge → rxcb`, ~27 ms median.** These are single-run observations from
one board pair at 20 cm on a bench; they are `M-HW` single-run measurements, not
distribution estimates for a population.

## Measurement limitations — read before quoting

1. **The RX `seq` field is NOT the BENCH58 sequence number.** It is read from the
   first byte of the ARQ-decoded frame at the RX-callback hook, which is an
   internal header byte. It reports a constant **69** on both boards, so the
   report's "sequence coverage" section is **void for RX** (the `unique=1` /
   `duplicates=[69]` lines are artifacts of this, not evidence of duplicate
   frames). The **TX** coverage (0..79, unique 80, gaps 0) is valid.
   This does **not** affect `edge → rxcb`: the accept hook reads the same variable
   within the same `process_rx_frame` call, so the pairing is self-consistent.
2. **One gross artifact exists** and is *not* excluded: DEVICE_B `edge → rxcb`
   max = 341,336.571 ms and `rxcb → accept` max = 265,880.270 ms. Mechanism: the
   ISR latches an edge, and if no RX callback consumes it (frame rejected before
   the callback) the stale latch is consumed by a much later frame, inflating the
   interval. Only 1 of 68 records is affected. Median and IQR are robust to it.
   The exclusion rule below was declared before analysis and excludes nothing —
   quoting the median is defensible, but **a reviewer should be told the extreme
   exists**.
3. One DEVICE_B RX record has zero timestamps and is reported as
   *not measurable* rather than dropped.
4. No lost sync pulses were reported for any record.

## Declared exclusion rule (fixed before analysis)

**None.** No frame or record is excluded for being a timing outlier. The full
distribution including extremes is reported, medians and IQRs are quoted as
robust statistics, and zero-timestamp records are reported as not-measurable
rather than silently removed.

## Build and conditions

- Binary: `build_cap_timing/ceepew.bin`,
  SHA-256 `b335daf4c879b9c5ddbe202a3636e8324e049d5e389d95eae80360441aa76bb0`,
  1,403,184 B, identical on both boards.
- Compiled flags verified in the generated header (not just `sdkconfig`):
  `CONFIG_CEEPEW_CAPTURE=1`, `CONFIG_CEEPEW_CAPTURE_MODE_CEEPEW=1`,
  `CONFIG_CEEPEW_BENCH_58B=1`, `CONFIG_CEEPEW_BENCH_TIMING=1`.
- Flashed with `esptool` directly to COM3 and COM4 (no rebuild), each reporting
  `Hash of data verified`; logs in `flash_COM3.log` / `flash_COM4.log`.
- Channel 1, hopping **off**, 115200 baud, 480 s, 20 cm, DIO / 40 MHz flash.
- First-cycle pairing succeeded (`POST_DERIVE_SYNC` = 3 completions,
  0 timeouts); BENCH58 campaign ran to completion (`BENCH58_DONE` = 2, one per
  board; `BENCH58_TX` = 175 including ARQ retransmissions).

## Implementation history (why the firmware is trustworthy here)

The instrumentation was generated by a first agent and then audited line by line;
**five defects were found and fixed before this run**, any one of which would have
produced a useless dataset:
1. `bench_timing_init()` was never called — GPIO14 unconfigured, no pulse ever
   emitted, every record zero. (fatal)
2. `bench_timing_pulse()` discarded its own timestamp — `pulse=` always 0, making
   `submit → pulse` and `pulse → sendcb` unmeasurable.
3. A board's own driven edge was consumed as the peer's pulse, corrupting
   `edge → rxcb`. Fixed with a self-edge guard window plus a pending-edge clear.
4. An 8-bit ring head counter against a 384-entry ring — would wrap at 256 and
   clobber the earliest records.
5. The module lived in `main/`, which components cannot include (build break).
A sixth problem was architectural: the original static ring needed **21,504 B**
against **~256 B** of spare `dram0_0_seg`, so it could never have linked at any
useful size. Records are therefore **streamed unbuffered** and printed at the
points where every interval they report is already frozen (TX at the send
callback, RX at frame accept), which cannot perturb a measurement.

## Provenance caveat

This run's binary was built **before** a separate fix to
`components/transport/transport_ble.c` (`BUG-BLE-CTX-LOCK-PANIC`, a mutex
release-on-timeout path that could panic and reboot a board during pairing). E5
was flashed via `esptool` from the already-built image, so **this run did not
include that fix** — and it did not need to: E5's pairing succeeded first cycle
with 0 timeouts. The E5 binary hash above identifies the exact image.

## Reproduce

```
python tools/e1_run_mode.py --run-id E5_sync_pulse_20261018 \
  --build-dir build_cap_timing --ports COM3,COM4 --baud 115200 \
  --duration 480 --no-flash
python tools/bench_latency_report.py --run-id E5_sync_pulse_20261018
```

# E5 Sync-Pulse Latency Analysis Tool

## Overview

`tools/bench_latency_report.py` analyzes E5 sync-pulse timing run logs and reports **one-way over-the-air latency statistics**.

## CLI Usage

```bash
# Analyze a run by ID (reads data/raw/<run-id>/capture_dual.log)
python tools/bench_latency_report.py --run-id E5_sync_pulse_20261019

# Analyze an explicit log file
python tools/bench_latency_report.py --log path/to/capture.log

# Run self-test (mandatory, proves correctness)
python tools/bench_latency_report.py --self-test

# Full options
python tools/bench_latency_report.py \
  --run-id E5_sync_pulse_20261019 \
  --derived-dir data/derived \
  --json data/derived/E5_sync_pulse_20261019/latency.json \
  --md data/derived/E5_sync_pulse_20261019/latency.md \
  --quiet \
  -v
```

### CLI Flags

| Flag | Description |
|------|-------------|
| `--run-id RUN_ID` | Run ID (reads `data/raw/<run-id>/capture_dual.log`) |
| `--log PATH` | Explicit path to log file (overrides `--run-id`) |
| `--derived-dir DIR` | Output directory for derived files (default: `data/derived`) |
| `--json PATH` | Write JSON output to this path |
| `--md PATH` | Write Markdown output to this path |
| `--quiet` | Suppress stdout report |
| `-v`, `--verbose` | Verbose output |
| `--self-test` | Run self-test and exit (mandatory correctness proof) |

## Input Format

The E5 firmware emits one line per timing record at the end of a campaign:

```
BENCH_TIME seq=<n> role=<tx|rx> submit=<u64> pulse=<u64> sendcb=<u64> edge=<u64> rxcb=<u64> accept=<u64>
```

Fields that do not apply to that record's role are `0`. Lines may be interleaved with ESP-IDF log output and prefixed with the monitor's host timestamp and device label (e.g. `[COM3]`).

**Defensive parsing:** Lines that do not match the full expected field set are skipped and counted. The report shows how many records were recovered vs skipped.

## Intervals Computed

### TX-side (same-clock, single device)

| Interval | Description |
|----------|-------------|
| `submit→pulse` | Time from frame submission to sync pulse emission |
| `pulse→sendcb` | Time from sync pulse to send callback |

### RX-side (same-clock, single device)

| Interval | Description |
|----------|-------------|
| `edge→rxcb` | **Headline one-way latency** — sync pulse edge to RX callback |
| `rxcb→accept` | Time from RX callback to frame acceptance |

## THE SAME-CLOCK RULE (Critical for Correctness)

**The two boards have INDEPENDENT microsecond clocks** (`esp_timer_get_time()` counts from each board's own boot; they are **NOT synchronised**).

Therefore:

1. **NEVER compute a difference between timestamps originating on different devices.** Report the device each timestamp came from.

2. **Every reported statistic MUST be a same-clock interval:**
   - TX-side (single device): `submit→pulse` and `pulse→sendcb`
   - RX-side (single device): `edge→rxcb` and `rxcb→accept`

3. **`edge→rxcb` is the headline one-way figure** — the sync pulse crosses a 20 cm wire in nanoseconds and the frame is handed to the radio immediately after the pulse, so this interval captures:
   - Radio TX
   - Propagation (over-the-air)
   - RX
   - Callback delivery
   
   All measured on the **receiver's own clock**.

4. If a record has a zero timestamp for a field needed by an interval, that interval is skipped and counted under a **"not measurable"** bucket — we never substitute another timestamp or a host timestamp.

## Hardware Facts the Tool Depends On

1. **Single shared wire:** GPIO14 connects the two boards for sync pulse transmission. The pulse crosses this wire in nanoseconds, making `edge→rxcb` a valid measure of one-way latency.

2. **Both boards transmit AND receive:** During a campaign, each board acts as both TX and RX. The tool groups statistics by device and role to capture both perspectives.

## Output Files

The tool writes to `data/derived/<run-id>/`:

- `latency.json` — Full statistics as JSON
- `latency.md` — Markdown report for the paper

## Self-Test

The `--self-test` flag runs synthetic tests that prove:

1. **(a)** A normal TX record and a normal RX record produce the four correct same-clock intervals with correct values.

2. **(b)** A record with `edge=0` lands in the "not measurable"/"lost pulse" bucket and does not corrupt other statistics.

3. **(c)** A malformed/truncated line is skipped and counted, not crashed on.

4. **(d)** **A cross-device subtraction is NEVER performed** — the tool is handed two devices with wildly different clock bases (1e6 µs apart) and still reports identical, correct intervals. All computed intervals are verified to be < 10ms, proving no cross-device mixing occurred.

**Exit code:** 0 on pass, 1 on failure.

## Interpretation Block

Every JSON output includes an `interpretation` field that documents the same-clock rule. This ensures a reviewer cannot mistake a cross-clock subtraction for a latency measurement.

## Statistics Reported

For each interval, the tool reports:

- **N** — Sample count
- **min** — Minimum value (µs and ms)
- **median** — 50th percentile (µs and ms)
- **IQR** — Interquartile range (q1, q3)
- **p95** — 95th percentile (µs and ms)
- **max** — Maximum value (µs and ms)
- **mean** — Arithmetic mean (µs and ms)

## Frame Accounting

The tool also reports:

- How many `seq` values were seen per device with `role=tx` vs `role=rx`
- Any `seq` seen twice (duplicates)
- Any `seq` missing (gaps in the expected 0..79 range for 80-frame campaigns)
- Any `seq` whose RX record has `edge=0` — reported explicitly as **lost sync pulse / no edge** (a distinct failure mode from a lost frame)
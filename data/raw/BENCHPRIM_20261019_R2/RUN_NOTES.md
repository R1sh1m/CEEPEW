# BENCHPRIM_20261019_R2 — on-device primitive micro-bench — confirmation re-capture — 2026-10-19

**Status: PRE-REGISTRATION written before the R2 capture.** Results are appended after the run,
before sealing.

## Why this run exists (declared before any R2 data was collected)
Run `BENCHPRIM_20261019` (attempt 1, sealed separately) produced all 150 emissions
(25 x 3 primitives x 2 devices) with `err=0` everywhere, but the 115200 console link mangled
three DEVICE_B lines in transit: two recovered exactly (`us=332039`, `us=663111`), one
partially (`ed25519_sign i=9 us=3318??`, range 331800-331899, median-insensitive; raw lines
quoted in the attempt-1 notes). The strict extractor reported 147/150 -> exit 1.

R2 is a confirmation re-capture, **not a replacement**:
- instrument unchanged (same flashed image
  `e1ca1b5a80efe6d70d8abbf54452e80b63bf95eae9717b6233d83832ff2930ce`, same `build_bench_prim`,
  same boards COM3/COM4);
- no re-flash: the monitor's reset pulse re-boots both boards, re-running the one-shot bench;
- identical measurement definition, N=25 per primitive per device (150 expected);
- R2 exists to (a) provide a complete set if the link behaves, (b) test reproducibility of the
  attempt-1 medians. Attempt 1 is kept and sealed regardless; R2 does not supersede it.

## Measurement and output format
As pre-registered in `BENCHPRIM_20261019/RUN_NOTES.md` (unchanged): `[BENCHPRIM] <primitive>
i=<i> us=<dt> err=<e>` lines, one 1-tick delay between samples, UART printed outside the
timed region, keys fresh at boot, timed calls = X25519 shared secret / Ed25519 sign /
Ed25519 verify of a 58-byte 0x41 message.

## Pre-registered exclusion rule (declared before any result)
**None**, identical to attempt 1. All samples reported including sample 0 and outliers;
`err!=0` counted, not dropped; data loss (if any) reported with raw lines quoted, values never
invented (same recovery pass as attempt 1, applied uniformly). If the log contains more than
one boot, that is reported; sample sets from distinct boots are never mixed.

## Procedure
- capture: `python tools/ceepew_monitor.py --port COM3 --port COM4 --baud 115200 --duration 120
  --log data/raw/BENCHPRIM_20261019_R2/capture_dual.log --no-color`
  (monitor open pulses the reset; boards re-boot; bench re-runs from sample 0)
- split: `python tools/capture/split_dual_log.py data/raw/BENCHPRIM_20261019_R2/capture_dual.log
  --outdir data/raw/BENCHPRIM_20261019_R2 --prefix capture`
- extract (strict, pre-registered contract): `python tools/scratch/benchprim_extract.py
  --run-id BENCHPRIM_20261019_R2`
- recover (only if UART mangling occurs): `python tools/scratch/benchprim_recover.py
  --run-id BENCHPRIM_20261019_R2`
- fields at seal: `n_completed` unit = one timed primitive call (expected 150 = 25 x 3 x 2).

## Results

**Run executed 2026-10-19**, immediately after attempt 1. Reset-only (no reflash): the monitor
open pulse re-booted both boards; one boot per device (`rst:` count = 1 in each split log); the
one-shot bench ran from sample 0 (`begin` 3383 ms A / 3478 ms B; `end` 32097 ms A / 32221 ms B).
120 s window as pre-registered.

### Completeness
Strict extractor: 150/150, no missing or duplicate indices, completeness PASS (exit 0). No UART
mangling occurred in this capture (recovery pass: 0 mangled lines).

### Raw samples as measured (us, `esp_timer_get_time`)

| device | primitive | n | min | median | max | err!=0 |
|---|---|---|---|---|---|---|
| DEVICE_A | x25519 | 25 | 147627 | 147746 | 147896 | 0 |
| DEVICE_A | ed25519_sign | 25 | 331578 | 331792 | 332198 | 0 |
| DEVICE_A | ed25519_verify | 25 | 662419 | 662686 | 663090 | 0 |
| DEVICE_B | x25519 | 25 | 147681 | 147785 | 148183 | 0 |
| DEVICE_B | ed25519_sign | 25 | 331546 | 331816 | 333474 | 0 |
| DEVICE_B | ed25519_verify | 25 | 662499 | 662762 | 663305 | 0 |

All 150 calls returned `err=0`. Device-to-device median agreement: 39 us (x25519), 24 us
(sign), 76 us (verify). The single high sign sample (B max 333474, +1.66 ms over median) is
retained per the no-exclusion rule.

### Reproducibility vs attempt 1 (median delta, R2 - attempt 1)
x25519: A +41 us, B +137 us; ed25519_sign: A -19 us, B -26 us; ed25519_verify: A +154 us,
B +155 us. Largest delta 155 us (~0.023 % of the verify median). Same image
(`e1ca1b5a80efe6d70d8abbf54452e80b63bf95eae9717b6233d83832ff2930ce`), same boards, same
conditions.

### Observations
- `[BENCHKAT] ascon=FAIL x25519=FAIL eddsa=FAIL` at boot, identical to attempt 1 and to every
  bench-build capture since `E1_B_ccmp_20261017` (see attempt-1 notes; BENCHKAT lane).
- The BENCH58 campaign also runs after the bench in the same boot (`[BENCH58_TX] seq=14-15`
  observed at ~111-118 s), including its replay arm (`ESL discard: replay detected`); unrelated
  to this measurement.
- The splitter's 4 skipped lines are the monitor's lifecycle lines
  (`Monitoring started.` / `Port closed.`), not sample data.

### Artifacts
`capture_dual.log`, `capture_DEVICE_A.log`, `capture_DEVICE_B.log`. Derived:
`data/derived/BENCHPRIM_20261019_R2/benchprim_samples.csv` and
`benchprim_samples_recovered.csv` (150 rows each).

### Seal fields
`n_completed = 150` (unit: one timed primitive call = 25 x 3 x 2). `n_excluded = 0`
(rule: None).

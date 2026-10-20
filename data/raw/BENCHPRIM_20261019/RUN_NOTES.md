# BENCHPRIM — on-device primitive micro-bench — 2026-10-19

**Status: PRE-REGISTRATION written before the run.** Results are appended after the run,
before sealing.

## Purpose
Produce a raw-sample, provenance-clean measurement of three on-device primitives to replace
registry entries `CYC-X25519` (218.75 ms, n=53) and `CYC-ED25519` (1075 ms, n=76), whose
`raw_log` fields point at data that does not exist on disk. No assumption that those numbers
reproduce: whatever is measured is reported as-is.

## What is measured (raw primitives, not the protocol path)
At session-task start, before any BLE/ESP-NOW traffic, the firmware times:
1. `crypto_ecdh_shared_secret()` — X25519, local priv × peer pub → 32-byte shared secret;
2. `crypto_eddsa_sign()` — Ed25519 sign of a 58-byte 'A'-filled message;
3. `crypto_eddsa_verify()` — Ed25519 verify of the same message.
N = 25 samples per primitive per device (150 samples total across two devices). Each sample:
`esp_timer_get_time()` immediately before/after the single primitive call; the UART line is
printed **after** the timed region; one 1-tick `vTaskDelay` between samples (outside the timed
region). Keys are generated fresh at boot (key values do not affect primitive cost).
msg_len = 58 B ('A' fill) matches the BENCH58 campaign payload.

## Output format (host-parsed)
```
[BENCHPRIM] begin n=25 msg_len=58
[BENCHPRIM] x25519 i=<i> us=<dt> err=<e>
[BENCHPRIM] ed25519_sign i=<i> us=<dt> err=<e>
[BENCHPRIM] ed25519_verify i=<i> us=<dt> err=<e>
[BENCHPRIM] end
```
Device attribution comes from the per-port split of the dual monitor log.

## Pre-registered exclusion rule (declared before any result)
**None.** All 25 samples per primitive per device are reported — including sample 0 (no
warm-up discard) and including any outliers/extremes. `err != 0` samples are counted and
reported, not dropped. A board that fails to emit a full set is reported as an apparatus
failure.

## Firmware gating
Compiled only under `CONFIG_CEEPEW_BENCH_TIMING=y` (`main/task_session.c`,
`bench_prim_run()`); it is NOT present in the plain capture build. No protocol, timing or
threshold was changed anywhere.

## Build provenance
- `build_bench_prim` is a fresh build directory whose `sdkconfig` is a byte-for-byte copy of
  the sealed `build_cap_timing` configuration (sha256
  `0e2573facaa0140713646583bce7ccdd9ad80dc3c5e3c7ba4ab7a67de40c1f83`) — the same
  configuration that produced the E5 reference image: `BENCH_TIMING=y`, `PIN_SYNC=14`,
  `CAPTURE_MODE_CEEPEW=y`, no `BENCH_58B`, no `DEVELOPMENT_MODE`.
- ESP-IDF **v6.0.2** (`C:\esp\v6.0.2\esp-idf`, tag v6.0.2), compiler
  `xtensa-esp-elf 15.2.0_20251204` — campaign toolchain (E1/E2a/E2b/E5/E3); v6.0.3 is not
  used (see E3 `PREREGISTRATION.md` §8: these capture builds cannot link on v6.0.3).
- The manifest records configured/generated sdkconfig hashes + the binary hash.

## Why this is not a capture-mode run
No RF behaviour is measured. The boards are flashed, a 120 s dual-port monitor log is taken,
and each board's bench completes ~30-40 s after boot.

## Commands
- flash: `idf.py -B build_bench_prim -DSDKCONFIG=build_bench_prim/sdkconfig -p COM3 flash`,
  then the same with `-p COM4` (v6.0.2 environment).
- monitor: `python tools/ceepew_monitor.py --port COM3 --port COM4 --baud 115200 --duration
  120 --log data\raw\BENCHPRIM_20261019\capture_dual.log --no-color`
- split: `python tools/capture/split_dual_log.py <dual log> --outdir
  data\raw\BENCHPRIM_20261019 --prefix capture`

## Post-registration instrument changes (2026-10-19 ~22:25, before any data was collected)
The pre-registration above is unchanged. Recorded facts:
1. **Compile fix before any data:** `bench_prim_run()` called
   `crypto_ecdh_generate_keypair()` / `crypto_eddsa_keypair()` without including
   `crypto_ecdh.h` / `crypto_eddsa.h`; the two includes were added. No statement inside any
   timed region was changed.
2. **Link-time DRAM failure and fix:** the first BENCH_TIMING link overflowed `dram0_0_seg`
   by 40 B. Fixed by right-sizing the capture ring payload cap 300 -> 288 B (frees 48 B;
   non-truncation argument in the E1_A notes: protocol bound 278 B, longest genuine DUT
   frame 269 B). This run takes no captures, so the ring is inert here. The capture
   instrument's row buffer lives on its own task stack (not `.bss`).
3. **Provenance:** configured `sdkconfig` sha256 `0e2573fa...` (pristine byte-copy of
   `build_cap_timing`), generated `config/sdkconfig.h` sha256 `9bdf5de9...` (identical to the
   sealed E5 image), binary hash recorded in the manifest. `idf_version` is recorded as
   `v6.0.2` (the manifest helper now git-describes the active IDF; it previously fell back
   to `unknown`).
4. **Reporting fields at seal:** `n_completed` unit = one timed primitive call (expected 150 =
   25 x 3 x 2). `n_excluded` = 0 by the rule above ("None"). The exclusion field is a
   **rule**, not a count.

## Results

**Run executed 2026-10-19.** One boot per device; the one-shot bench was captured from sample 0
(`[BENCHPRIM] begin` at 3492 ms A / 3387 ms B; `end` at 32179 ms A / 32120 ms B).

### Execution deviations (recorded, not silent)
1. **Flash tool.** The pre-registered `idf.py ... flash` was not used. Flash was applied with
   esptool at 115200. A first invocation with the esptool options placed after the `write-flash`
   subcommand exited rc=2 before writing; its console output was overwritten by the successful
   invocation. The successful invocation wrote and verified both boards (`Hash of data
   verified`, `Staying in bootloader`); `flash_COM3.log` / `flash_COM4.log` contain that
   successful run (esptool v5.3.1; `--after no_reset` accepted with a deprecation warning).
2. **Capture window.** 180 s (pre-registered 120 s), extended for completeness margin; no
   measurement parameter changed.
3. **Pre-registration prose discrepancy (`BENCH_58B`).** The Build provenance section above
   states "no `BENCH_58B`". The configuration actually used contains
   `CONFIG_CEEPEW_BENCH_58B=1` (same as the sealed E5 `build_cap_timing`), so the BENCH58
   campaign also runs later in the same boot (observed at `seq=26-27` by 172-180 s). The bench
   is a synchronous one-shot at session-task start: no session traffic occurs during the timed
   window. BLE discovery/scanning from the other task is active during part of the window (gap
   events from ~10 s); dispersion is tight and both devices agree. This changes neither the
   measurement definition nor the sample treatment.

### Completeness (pre-registered contract)
Strict extractor (`tools/scratch/benchprim_extract.py`): 147/150 parsed -> exit 1.
All 150 timed calls were emitted (25 indices x 3 primitives x 2 devices; `begin`/`end` on both
devices). Three DEVICE_B lines were mangled by 115200-console character loss in transit; the
samples are present in the raw per-device log:

```
[DEVICE_B] ... [BENCHPRIM] ed25519_sign   i=9  us=3318 e=0
[DEVICE_B] ... [BENCHPRIM] ed25519_sign   i=12 us=332039rr=0
[DEVICE_B] ... [BENCHPRIM] ed25519_verify i=23 us663111 err=0
```

Recovery pass (`tools/scratch/benchprim_recover.py`; no digits invented): two lines recovered
exactly (`332039`, `663111`); the third lost two digits and is reported as the range
`[331800, 331899]` (prefix `3318`), median unchanged at either bound. Usable: 149 exact values
+ 1 bounded. Nothing was dropped by an exclusion rule (rule: None).

### Raw samples as measured (us, `esp_timer_get_time`)

| device | primitive | n usable | min | median | max | err!=0 |
|---|---|---|---|---|---|---|
| DEVICE_A | x25519 | 25 | 147609 | 147705 | 147891 | 0 |
| DEVICE_A | ed25519_sign | 25 | 331650 | 331811 | 332247 | 0 |
| DEVICE_A | ed25519_verify | 25 | 662176 | 662532 | 662877 | 0 |
| DEVICE_B | x25519 | 25 | 147587 | 147648 | 147846 | 0 |
| DEVICE_B | ed25519_sign | 24 (+1 bounded) | 331559 | 331842 | 332407 | 0 |
| DEVICE_B | ed25519_verify | 25 | 662213 | 662607 | 663111 | 0 |

All 150 calls returned `err=0`. Device-to-device median agreement: 57 us (x25519), 31 us
(sign), 75 us (verify).

### Cross-run note
A confirmation re-capture was run immediately afterwards as a separate, separately
pre-registered run (`BENCHPRIM_20261019_R2`; reset-only, same image, 120 s). R2 produced a
strict 150/150 set and reproduces the medians above within 155 us (see R2 notes). Attempt 1 is
kept and sealed as-is; R2 does not replace it.

### Observations for other lanes (not this run's measurement)
- `[BENCHKAT] ascon=FAIL x25519=FAIL eddsa=FAIL` at boot on both devices. Pre-existing:
  identical on every bench-build capture in `data/raw` since `E1_B_ccmp_20261017` (40 files).
  The live crypto path works in the same boots (all BENCHPRIM samples err=0; BENCHTX/BENCHRX
  stage timings err=0; sessions establish and exchange traffic). Root cause not investigated
  here -> BENCHKAT lane.
- The splitter's "4 line(s) had no [LABEL] wrapper and were skipped" are the monitor's own
  lifecycle lines (`Monitoring started.` / `Port closed.`), not sample data.
- `Epoch sync` and `ESL discard: replay detected` lines in the same logs belong to the BENCH58
  campaign (its replay arm), not to this measurement.

### Artifacts
`capture_dual.log`, `capture_DEVICE_A.log`, `capture_DEVICE_B.log`, `flash_COM3.log`,
`flash_COM4.log`. Derived: `data/derived/BENCHPRIM_20261019/benchprim_samples.csv` (strict,
147 rows) and `benchprim_samples_recovered.csv` (150 rows with per-row status).

### Seal fields
`n_completed = 150` (unit: one timed primitive call emitted = 25 x 3 x 2).
`n_excluded = 0` (rule: None). Caveat carried here: 1 of 150 values is range-bounded only
(transit corruption); 2 recovered exactly; 149 exact values.

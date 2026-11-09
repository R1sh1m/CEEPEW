# CEE-PEW measurement campaign tooling

Every quantitative claim in the manuscript must be traceable to a sealed
campaign manifest. This directory implements that chain. Pure standard
library, no network access, no third-party dependencies.

```
manifest.py         emit and verify a sealed campaign manifest
registry_append.py  append registry entries that are bound to a manifest
run_x1_timing.py    per-stage timing analysis, cluster-bootstrap intervals (campaign X1)
run_x12_aead.py     same-board AEAD and signature baseline (campaign X12)
run_x8_delivery.py  delivery accounting with an explicit drop breakdown (campaign X8)
build_matrix.py     firmware build matrix orchestrator across configurations
```

---

## Campaign Spectrum & Hardware Matrix

| Campaign | Primary Purpose & Measurement | Target Metrics | Hardware Setup |
|---|---|---|---|
| **E1 Baseline** | Comparative 802.11 wire exposure and airtime | 249 B MPDU, 2.84 ms airtime, $7.99\text{ bits/B}$ entropy | Dual ESP32 (`COM3`, `COM4`) @ 20 cm + `COM6` sniffer |
| **E2 Fault Injection** | Fail-closed isolation of forged tags & corrupted frames | 100% dropped at Ascon tag verify (`err=5`); 0 to UI | Device A (`COM3`) injecting, Device B (`COM4`) receiver |
| **E3 Anti-Replay** | 64-bit sliding window anti-replay validation | 100/100 vectors rejected (95% CI upper bound 3%) | Dual ESP32 with replay test vectors |
| **E5 Sync Pulse** | Sub-microsecond hardware cross-board timing | Median 27.0 ms round-trip cross-board latency | Copper sync wire connecting GPIO14 to GPIO14 |
| **BENCHPRIM** | Boot-time isolated cryptographic micro-benchmarks | X25519 (148.0 ms), Ed25519 sign (332.2 ms), verify (663.2 ms) | Dual ESP32 DevKit v1 @ locked 160 MHz |
| **X1 Timing** | Full 16-stage end-to-end pipeline execution timing | Per-stage microsecond durations with cluster bootstrap CIs | Dual ESP32 (`COM3`, `COM4`), GPIO14 sync wired |
| **X2 AEAD vs Asym**| Scaling of symmetric AEAD vs asymmetric Ed25519 | Ascon encrypt (41 µs), decrypt (327 µs) vs Ed25519 fixed | Dual ESP32 DevKit v1 @ locked 160 MHz |
| **X4 Tamper** | ESL header bit manipulation and MAC fault injection | 100% header drops at parser; 100% tag drops at Ascon | Device A (`COM3`) injecting, Device B (`COM4`) receiver |
| **X5 DoS Stress** | Resilience to high-frequency frame floods (10–500 fps) | 0% OLED UI jitter; clean bounded queue drop; 0 leaks | Dual ESP32 with external Action frame flood injector |
| **X6 FEC Ablation**| Hamming(15,11) FEC enabled vs compiled out | 249 B vs 187 B wire size; 100% 1-bit corrections | Dual ESP32 flashed with `build_fec2` vs `build` |
| **X7 Channel Hop** | Channel switching latency & pseudo-random rendezvous | < 15 ms hop dwell; < 2% frame loss during hopping | Dual ESP32 hopping channels 1, 6, 11 + `COM6` sniffer |
| **X8 Delivery** | Frame delivery accounting & loss classification | Disentangled PHY drops vs cryptographic tag rejections | Dual ESP32 with RF attenuator + `COM6` sniffer |
| **X9 Zero-Heap** | Region bump allocator watermark & stack headroom | 0 bytes heap; 38.4 KB / 48 KB pool; > 3.2 KB stack margin | Dual ESP32 instrumented memory builds |
| **X10 OTA PCAP** | Clean promiscuous 802.11 Wireshark capture generation | 249 B action frames; 0 cleartext bytes; verified dissector | Dual ESP32 (`COM3`, `COM4`) + dedicated sniffer (`COM6`) |
| **X11 Range Sweep**| Multi-distance RSSI attenuation (10 cm to 20 m) | RSSI (-28 to -88 dBm), PDR across distances | Stationary `COM3` + mobile `COM4` at 10 cm–20 m |
| **X12 Timing CI** | Large-scale statistical confidence interval sampling | 10,000 bootstrap resamples on $N \ge 100$ trials | Dual ESP32 DevKit v1 @ locked 160 MHz |

---

## The Rule

A measurement that is not bound to a manifest does not enter
`results/registry.json`. `registry_append.py` enforces this: it re-hashes
every artifact of the manifest before it writes anything, and it refuses an
entry whose cited artifact is not listed in the manifest.

---

## Campaign Workflow

1. **Build** the firmware for the campaign configuration and note the build
   directory. Every campaign pins the configuration through `sdkconfig`,
   whose SHA-256 goes into the manifest. Use the matrix driver rather than
   `-D CONFIG_*` on the command line: `idf.py` regenerates `sdkconfig` from
   the defaults files and silently ignores configuration passed through the
   CMake cache, so `-D CONFIG_CEEPEW_BENCH_AEAD=y` produces a shipping image
   and a campaign that measures the wrong binary (ISS-T07).

   ```powershell
   $env:IDF_PATH='C:\esp\v6.1\esp-idf'; $env:IDF_TOOLS_PATH='C:\Espressif\tools'
   $env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python_env\idf6.0_py3.13_env'
   & 'C:\esp\v6.1\esp-idf\export.ps1' | Out-Null
   python tools/campaign/build_matrix.py --configs bench
   ```

   The driver deletes any stale private `sdkconfig` first, so the overlay is
   authoritative, then verifies the ELF symbols and `sdkconfig.h` against the
   configuration before it records the build as OK. A build that compiles but
   carries the wrong symbols is recorded FAILED.

2. **Flash** both boards and capture the serial logs into
   `data/raw/<campaign_id>/`.

3. **Seal** the run: emit the manifest with the git revision, the sdkconfig
   hashes, the board identities, the firmware hash and the hash of every
   captured artifact.

   ```powershell
   python tools/campaign/manifest.py --campaign X12_aead_20261022 `
     --board "Device A:COM3:8c:94:df:97:d0:c0" `
     --board "Device B:COM4:1c:c3:ab:c2:52:ac" `
     --runner tools/campaign/run_x12_aead.py `
     --firmware build/ceepew.bin --build-dir build `
     --artifact data/raw/X12_aead_20261022/capture_DEVICE_A.log `
     --artifact data/raw/X12_aead_20261022/capture_DEVICE_B.log
   ```

4. **Analyse** with the campaign runner. The runner reads the sealed logs
   and prints the table; it never writes into the registry.

   ```powershell
   python tools/campaign/run_x12_aead.py `
     --log data/raw/X12_aead_20261022/capture_DEVICE_A.log `
     --log data/raw/X12_aead_20261022/capture_DEVICE_B.log `
     --out-json data/derived/X12_aead_20261022.json
   ```

5. **Record** each number in the registry, bound to the manifest.

   ```powershell
   python tools/campaign/registry_append.py --manifest data/manifests/X12_aead_20261022.json `
     --id X12-ASCON128-ENC-MEDIAN --description "Ascon-128 encrypt median, 60 B" `
     --value 41 --unit us --class M-HW --n 30 `
     --artifact data/raw/X12_aead_20261022/capture_DEVICE_A.log `
     --script tools/campaign/run_x12_aead.py --reference "Section VI.B"
   ```

6. **Verify** the whole chain at any time:

   ```powershell
   python tools/campaign/registry_append.py --verify-all
   ```

---

## Measurement Hygiene

* **CPU frequency.** `CONFIG_PM_ENABLE` is off and
  `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`, so the core does not downclock.
  The benchmark module reads the live clock through
  `esp_clk_tree_src_get_freq_hz()` and stores it in every record, so a run
  at an unexpected frequency is self-documenting.
* **Hardware acceleration is labelled.** Every benchmark case carries
  `hw_aes=1` only when the ESP32 AES accelerator is in the execution path.
* **Unavailability is reported, not hidden.** A case the build does not
  provide is emitted with `available=0` and appears in the table as such.
* **Intervals come from one place.** `tools/journal/stats_core.py` computes
  every interval (Wilson, Clopper-Pearson, cluster bootstrap, TOST, Holm).
  The campaign runners call it; they never hand-compute an interval.
* **Loss is attributed, not rounded away.** The delivery runner reports
  message-level failures by cause and reports a cause with no instrumented
  log line as "not instrumented" rather than folding it into "loss".

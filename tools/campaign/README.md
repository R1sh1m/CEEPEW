# CEE-PEW measurement campaign tooling

Every quantitative claim in the manuscript must be traceable to a sealed
campaign manifest. This directory implements that chain. Pure standard
library, no network access, no third-party dependencies.

```
manifest.py         emit and verify a sealed campaign manifest
registry_append.py  append registry entries that are bound to a manifest
run_x1_timing.py    per-stage timing analysis (cluster-bootstrap intervals)
run_x2_aead.py      same-board AEAD and signature baseline (campaign X2)
run_x8_delivery.py  delivery accounting with an explicit drop breakdown
```

## The rule

A measurement that is not bound to a manifest does not enter
`results/registry.json`. `registry_append.py` enforces this: it re-hashes
every artifact of the manifest before it writes anything, and it refuses an
entry whose cited artifact is not listed in the manifest.

## Campaign workflow

1. **Build** the firmware for the campaign configuration and note the build
   directory. Every campaign pins the configuration through `sdkconfig`,
   whose SHA-256 goes into the manifest.

   ```powershell
   $env:IDF_PATH='C:\esp\v6.0.3\esp-idf'; $env:IDF_TOOLS_PATH='C:\Espressif\tools'
   $env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python_env\idf6.0_py3.13_env'
   & 'C:\esp\v6.0.3\esp-idf\export.ps1' | Out-Null
   idf.py -B build_bench -D CONFIG_CEEPEW_BENCH_AEAD=y build
   ```

2. **Flash** both boards and capture the serial logs into
   `data/raw/<campaign_id>/`.

3. **Seal** the run: emit the manifest with the git revision, the sdkconfig
   hashes, the board identities, the firmware hash and the hash of every
   captured artifact.

   ```powershell
   python tools/campaign/manifest.py --campaign X2_aead_20261022 `
     --board "Device A:COM3:8c:94:df:97:d0:c0" `
     --board "Device B:COM4:1c:c3:ab:c2:52:ac" `
     --runner tools/campaign/run_x2_aead.py `
     --firmware build_bench/ceepew.bin --build-dir build_bench `
     --artifact data/raw/X2_aead_20261022/capture_DEVICE_A.log `
     --artifact data/raw/X2_aead_20261022/capture_DEVICE_B.log
   ```

4. **Analyse** with the campaign runner. The runner reads the sealed logs
   and prints the table; it never writes into the registry.

   ```powershell
   python tools/campaign/run_x2_aead.py `
     --log data/raw/X2_aead_20261022/capture_DEVICE_A.log `
     --log data/raw/X2_aead_20261022/capture_DEVICE_B.log `
     --out-json data/derived/X2_aead_20261022.json
   ```

5. **Record** each number in the registry, bound to the manifest.

   ```powershell
   python tools/campaign/registry_append.py --manifest data/manifests/X2_aead_20261022.json `
     --id X2-ASCON128-ENC-MEDIAN --description "Ascon-128 encrypt median, 60 B" `
     --value 41 --unit us --class M-HW --n 30 `
     --artifact data/raw/X2_aead_20261022/capture_DEVICE_A.log `
     --script tools/campaign/run_x2_aead.py --reference "Section VI.B"
   ```

6. **Verify** the whole chain at any time:

   ```powershell
   python tools/campaign/registry_append.py --verify-all
   ```

## Measurement hygiene

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

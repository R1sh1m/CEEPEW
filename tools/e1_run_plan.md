# E1 Capture Run Script

## Usage
```powershell
# E1-A: Plain ESP-NOW
python tools/e1_run_mode.py --run-id E1_A_plain_20261018 --build-dir build_cap_plain --ports COM3,COM5 --baud 921600 --duration 300 --flash

# E1-B: CCMP mode
python tools/e1_run_mode.py --run-id E1_B_ccmp_20261018 --build-dir build_cap_ccmp --ports COM3,COM5 --baud 921600 --duration 300 --flash

# E1-C: CEE-PEW full
python tools/e1_run_mode.py --run-id E1_C_ceepew_20261018 --build-dir build_cap_ceepew --ports COM3,COM5 --baud 921600 --duration 300 --flash
```

## Run Sequence
1. **E1-A (Plain)** - ~5 min flash + ~7 min capture = 12 min
2. **E1-B (CCMP)** - ~5 min flash + ~7 min capture = 12 min  
3. **E1-C (CEE-PEW)** - ~5 min flash + ~7 min capture = 12 min

**Total: ~36 min** (assuming boards free, builds complete)

## Post-Run Analysis
After each run completes:
```powershell
# Seal manifest with SHA-256 hashes
python tools/bench_manifest.py seal --run-id E1_A_plain_20261018
python tools/bench_manifest.py seal --run-id E1_B_ccmp_20261018
python tools/bench_manifest.py seal --run-id E1_C_ceepew_20261018

# Generate mode reports
python tools/capture/mode_report.py --run-id E1_A_plain_20261018
python tools/capture/mode_report.py --run-id E1_B_ccmp_20261018
python tools/capture/mode_report.py --run-id E1_C_ceepew_20261018

# Generate timing reports (BENCH58 logs)
python tools/bench_timing_report.py --run-id E1_A_plain_20261018
python tools/bench_timing_report.py --run-id E1_B_ccmp_20261018
python tools/bench_timing_report.py --run-id E1_C_ceepew_20261018
```

## Evidence Checklist
For each mode, verify:
- [ ] 80 frames transmitted (BENCH58_TX count)
- [ ] ~72 frames captured (8 forged excluded from peer capture)
- [ ] 10/10 forged frames rejected (E1-B, E1-C only)
- [ ] 0 forged frames reach "Message received" app layer
- [ ] Manifest sealed with SHA-256 hashes
- [ ] Registry entry added with run metadata

## Exclusion Rules (Written Pre-Run)
1. Hardware-ACK subtype 0x1d frames excluded from length/entropy analysis
2. Truncated captures (<28 B) flagged but NOT excluded from rejection counts
3. No frame excluded for being a timing outlier
4. Capture-integrity gap = TX count minus captured frames, reported with cause
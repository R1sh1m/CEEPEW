# tools/

Helper scripts for CEE-PEW development, testing, and debugging.
None are referenced by the firmware build.

## Contents

### 1. Master Pipeline & Firmware Testing
| File | Purpose |
|------|---------|
| `ceepew_pipeline.py` | **Master Pipeline Driver** — Single command interface for host tests, on-device diagnostics, pairing tests, production audit, and log analysis |
| `ceepew_monitor.py` | Unified serial monitor — single or dual port, color-coded output, log saving, diagnostic report detection |
| `ceepew_diagnose.ps1` | Unified test driver — on-device diagnostics, pairing tests, or build-only mode |
| `ceepew_log_pipeline.py` | Post-hoc log ingestion + signature analysis — tags known failure signatures, tracks across runs, generates findings report |
| `ceepew_production_check.py` | Audits deployment configuration (eFuse HMAC, Secure Boot v2, Flash Encryption, dev mode) |
| `flash_both.ps1` | Flash firmware to two ESP32 devices sequentially, then monitor the first |
| `setup_esp_idf.ps1` | ESP-IDF 6.0.2 environment initialization helper |
| `run_fuzz_harnesses.sh` | Shell runner for host libFuzzer/AFL target harnesses |
| `wrap_rsp_in_group.cmake` | CMake helper — wraps linker response files in `--start-group`/`--end-group` |

### 2. Paper Results & Claim Verification
| File | Purpose |
|------|---------|
| `recompute_paper_numbers.py` | **Claim & Arithmetic Verifier** — Recomputes 45 paper claims against formulas and `results/registry.json` |
| `bench_manifest.py` | **Dataset Provenance Sealer** — Generates and verifies cryptographic SHA-256 manifests for raw test runs |
| `verify_citations.py` | **Citation Auditor** — Queries Crossref to validate DOIs, author lists, and publication years |
| `paper_lint.py` | Lints paper text for canonical terminology, banned claims, and IEEE formatting |

### 3. Empirical Experiment Drivers & Analysis
| File | Purpose |
|------|---------|
| `e1_run_mode.py` | Experiment E1 driver: executes baseline vs CCMP vs CEE-PEW framing trials |
| `e1_frame_lengths.py` | Analyzes airtime, frame length distribution, and overhead ratios |
| `e2_forge_reject.py` | Experiment E2 driver: analyzes rejection of over-the-air forged frames |
| `e3_replay_reject.py` | Experiment E3 driver: analyzes rejection of sliding window replay frames |
| `p4a_delivery_measurement.py` | Evaluates packet delivery rate (PDR), ARQ retry histograms, and channel goodput |
| `bench_latency_report.py` | Aggregates microsecond-resolution hardware pipeline stage latencies |
| `bench_timing_report.py` | Summarizes on-device CPU execution timings across crypto operations |
| `promisc_capture_helper.py` | ESP32 promiscuous mode 802.11 sniffer capture orchestrator |

### 4. Protocol Dissectors & Calculators
| File | Location | Purpose |
|------|----------|---------|
| `ceepew_espnow.lua` | `tools/capture/wireshark/` | Wireshark Lua dissector for CEE-PEW ESL encapsulation |
| `airtime_calc.py` | `tools/wireshark/` | 802.11 PHY layer airtime and preamble calculation engine |
| `v2_efficiency.py` | `tools/wireshark/` | ESP-NOW v2.0 wire efficiency and payload packing model |

### 5. Directories & Sub-packages
| Directory | Purpose |
|-----------|---------|
| `tools/archive/` | Historical, completed, or single-use scripts preserved for provenance |
| `tools/scratch/` | Gitignored local workspace for ad-hoc probe scripts (never commit) |
| `tools/capture/` | Raw promiscuous sniffer capture tools, analyzers, and Wireshark dissectors |
| `tools/wireshark/`| Standalone mathematical models for airtime and 802.11 wire budgets |

---

## `ceepew_pipeline.py`

Master test & debug pipeline orchestrator.

### Usage

```pwsh
# Run native host unit test suite
python tools/ceepew_pipeline.py --mode host

# Run on-device diagnostics & auto-ingest logs
python tools/ceepew_pipeline.py --mode diag

# Run dual-device pairing test
python tools/ceepew_pipeline.py --mode pairing

# Perform production readiness check
python tools/ceepew_pipeline.py --mode prod

# Run complete verification suite
python tools/ceepew_pipeline.py --mode all

# List connected serial ports
python tools/ceepew_pipeline.py --list-ports
```

---

## `ceepew_monitor.py`

Python serial monitor supporting single-port and dual-port modes.
Automatically color-codes output by log severity when connected to a TTY.

### Color scheme

| Pattern | Color |
|---------|-------|
| `[E]`, `ERROR` | Red bold |
| `[W]`, `WARN` | Yellow |
| `[PASS]` | Green |
| `[FAIL]`, ` FAIL ` | Red bold |
| `[MILESTONE]` | Cyan |
| `[I]` | Default |
| `[D]`, `[V]` | Dim |

### Usage

```pwsh
# Single-port monitor for 60 seconds
python tools/ceepew_monitor.py --port COM5 --duration 60

# Dual-port monitor with combined log
python tools/ceepew_monitor.py --port COM5 --port COM6 --log session.txt

# Dual-port with per-port log files
python tools/ceepew_monitor.py --port COM5 --port COM6 --log-per-port

# Monitor until diagnostic report completes
python tools/ceepew_monitor.py --port COM5 --watch-diag --duration 60

# Disable ANSI color (e.g., when piping output)
python tools/ceepew_monitor.py --port COM5 --no-color
```

### Log file format

Lines are prefixed with elapsed seconds since session start:

```
0.5 [DEVICE_A] I (12345) session_fsm: Peer WiFi MAC registered: AA:BB:CC:DD:EE:FF
1.2 [DEVICE_B] I (12345) session_fsm: Rendezvous: SYNCED, offset=42 us
3.7 [DEVICE_A] E (12345) session_fsm: CRITICAL: SESSION_ESTABLISHED not delivered
```

---

## `ceepew_diagnose.ps1`

Unified test driver with three modes.

### Prerequisites

- The repository setup script at `.\tools\setup_esp_idf.ps1` (ESP-IDF 6.0.2)
- Devices connected on COM5 (and COM6 for pairing mode)

### Diagnose mode

On-device diagnostic: enables `CONFIG_CEEPEW_DEVELOPMENT_MODE`, builds, flashes,
monitors for the `=== DIAGNOSTIC REPORT ===` block, parses PASS/FAIL results,
and restores the sdkconfig.

```pwsh
.\tools\ceepew_diagnose.ps1 -Mode Diagnose
.\tools\ceepew_diagnose.ps1 -Mode Diagnose -Port COM5 -Duration 120 -FullClean
```

### Pairing mode

Two-device pairing test: builds, flashes both COM5/COM6, monitors both ports,
extracts key events (MILESTONE, PASS, FAIL, PAIR_FAIL, PHASE3, etc.) to a CSV summary.

```pwsh
.\tools\ceepew_diagnose.ps1 -Mode Pairing
.\tools\ceepew_diagnose.ps1 -Mode Pairing -Duration 300
.\tools\ceepew_diagnose.ps1 -Mode Pairing -MonitorOnly        # existing firmware only
.\tools\ceepew_diagnose.ps1 -Mode Pairing -SkipFlash           # skip build + flash
.\tools\ceepew_diagnose.ps1 -Mode Pairing -NoBuild             # flash existing build
```

### Build mode

Build only, with optional flash shortcuts.

```pwsh
.\tools\ceepew_diagnose.ps1 -Mode Build                          # build + print manual test instructions
.\tools\ceepew_diagnose.ps1 -Mode Build -ForceFlash               # build + flash both devices
.\tools\ceepew_diagnose.ps1 -Mode Build -ForceMonitor             # build + flash + monitor one device
```

## `ceepew_log_pipeline.py`

Post-hoc log ingestion and signature analysis. Ingests device monitor logs
(generated by `ceepew_monitor.py`), tags known failure signatures from
`ceepew_assert.c`, `task_session.c`, etc., tracks them across runs in a
persistent JSON store, and generates a Markdown findings report at the project
root (`DEVICE_LOG_FINDINGS.md`) — designed as a companion to
`DEBUG_LOG_OLED_I2C.md`.

### Workflow

```
pairing test runs → ceepew_monitor.py saves combined log → ceepew_log_pipeline.py ingest → DEVICE_LOG_FINDINGS.md
```

### Usage

```pwsh
# Ingest a combined log (generated by diagnose.ps1 or ceepew_monitor.py --log)
python tools/ceepew_log_pipeline.py ingest logs/pairing_20260715_173342.txt

# Same, but label the session for readability in the report
python tools/ceepew_log_pipeline.py ingest logs/pairing_20260715_173342.txt --session-name "pairing_test_01"

# Ingest + regenerate the findings report in one step
python tools/ceepew_log_pipeline.py ingest logs/pairing_20260715_173342.txt --report

# Regenerate the findings report from the existing store (no new ingest)
python tools/ceepew_log_pipeline.py report
```

### Input formats

The pipeline accepts both **combined** and **per-port** log formats from
`ceepew_monitor.py`:

| Format | Example | Source |
|--------|---------|--------|
| Combined | `0.5 [DEVICE_A] I (12345) task_session: msg` | `--log` combined file |
| Per-port | `0.5 I (12345) task_session: msg` | `--log-per-port` per-port files |

### Signature library

Built-in signatures are defined in the `SIGNATURES` list in the script.
Each signature maps a regex pattern to a category, severity, and description.
New patterns should be added as they are confirmed from real device logs.
The "Unclassified Anomalies" section of the report flags `E`/`W` lines that
didn't match any signature — this is the primary discovery surface for new
bugs.

### Files

| Artifact | Location | Purpose |
|----------|----------|---------|
| JSON store | `ceepew_log_store.json` (project root) | Persistent cross-run tracking of signature hits and anomalies |
| Findings report | `DEVICE_LOG_FINDINGS.md` (project root) | Generated Markdown report for agent handoff |

---

## `flash_both.ps1`

Flash two ESP32 devices sequentially, then start a serial monitor on the first port.

```pwsh
.\tools\flash_both.ps1                      # defaults: COM3 + COM4
.\tools\flash_both.ps1 -Port1 COM5 -Port2 COM6
```

---

## `wrap_rsp_in_group.cmake`

Standalone CMake helper (not currently included from the root `CMakeLists.txt`).
Wraps the linker response file in `-Wl,--start-group` / `-Wl,--end-group`
to prevent one-pass GNU ld from skipping libraries.
Not intended to be invoked directly.

---

## `recompute_paper_numbers.py`

Automated arithmetic, wire budget, airtime, and goodput checker. Recomputes all 45 quantitative paper claims against the formulas and the single source of truth in `results/registry.json`.

```pwsh
python tools/recompute_paper_numbers.py
```

Outputs a comparison table detailing claim status (`MATCH` vs `MISMATCH`).

---

## `bench_manifest.py`

Dataset provenance and integrity sealer for experimental captures in `data/raw/`.

```pwsh
# Create / seal a manifest for a capture run
python tools/bench_manifest.py seal --run-id E1_B_ccmp_20261017

# Cryptographically verify all SHA-256 sums in a sealed run
python tools/bench_manifest.py verify --run-id E1_B_ccmp_20261017
```

---

## `verify_citations.py`

Adversarial citation verification tool querying Crossref API to confirm publication metadata (DOI, title, authors, publication year).

```pwsh
python tools/verify_citations.py --bib docs/paper/references.bib
```


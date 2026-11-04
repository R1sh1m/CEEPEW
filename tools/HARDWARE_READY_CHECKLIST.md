# CEE-PEW Hardware Ready Checklist

**Status:** Ready for hardware testing when COM ports are known  
**Generated:** 2026-11-04  
**Project:** CEE-PEW Embedded Firmware Workspace

---

## Pre-Flight Checks (Completed)

- [x] **Build artifacts present** - `build/ceepew.bin` exists (1425632 bytes, 2026-11-04 02:45:27)
- [x] **Bootloader present** - `build/bootloader/bootloader.bin` exists (26240 bytes)
- [x] **Partition table present** - `build/partition_table/partition-table.bin` exists (3072 bytes)
- [x] **Flash script ready** - `tools/flash_both.ps1` (701 lines) - auto-detects ports, builds, flashes both in parallel
- [x] **Pipeline ready** - `tools/ceepew_pipeline.py` supports `--mode pairing`, `--mode diag`
- [x] **Log pipeline ready** - `tools/ceepew_log_pipeline.py` for ingestion and analysis
- [x] **Monitoring tools ready** - `tools/ceepew_monitor.py`, `tools/ceepew_diagnose.ps1`

---

## Hardware Setup Requirements

1. **Two ESP32 DevKits** (both connected via USB)
2. **CP210x/CH340 USB-UART drivers** installed
3. **ESP-IDF v6.0.2 environment** set up (script at `tools/setup_esp_idf.ps1`)
4. **Python environment** with required packages (pyserial)
5. **USB cables** that support data transfer (not charge-only)

---

## Step-by-Step Hardware Testing Procedure

### Step 1: Detect Connected Ports
First, identify which COM ports your ESP32 boards are connected to.

```powershell
# List all ESP32 serial ports
.\tools\flash_both.ps1 -ListPorts
```

Or using the pipeline:

```powershell
python tools/ceepew_pipeline.py --list-ports
```

**Expected output:** Two COM ports (e.g., `COM5`, `COM6`) with CP210x/Silicon Labs identifiers.

**Action:** Note these ports down as `PORT1` and `PORT2`.

---

### Step 2: Flash Both Boards
Flash both devices with the current build. Replace `COM5` and `COM6` with your actual ports.

#### Option A: Using flash_both.ps1 (Recommended)
```powershell
# Auto-detect ports (if exactly 2 detected)
.\tools\flash_both.ps1

# Or specify ports explicitly
.\tools\flash_both.ps1 -Port1 COM5 -Port2 COM6

# Skip build if you just want to flash existing binaries
.\tools\flash_both.ps1 -Port1 COM5 -Port2 COM6 -SkipBuild -NoMonitor
```

#### Option B: Using pipeline (for pairing test which includes flashing)
```powershell
python tools/ceepew_pipeline.py --mode pairing --port1 COM5 --port2 COM6
```

**What to look for:**
- `[PORT] OK (time)` for both devices
- "Both devices flashed successfully"
- No flash errors

---

### Step 3: Run Pairing Integration Test
The pairing test validates the full BLE discovery → pairing → ESP-NOW handoff flow.

```powershell
python tools/ceepew_pipeline.py --mode pairing --port1 COM5 --port2 COM6
```

**Duration:** ~120 seconds by default (can adjust with `--duration`)

**What to look for on serial output:**
- `[PAIRING_STEP]` messages showing state progression
- `[PAIRING]` BLE discovery and GATT exchange
- `HANDOFF_READY sync complete` 
- `=== DIAGNOSTIC REPORT ===` or session establishment
- No `[PAIRING_TIMEOUT]` errors

**Logs saved to:** `logs/pairing_YYYYMMDD_HHMMSS.txt`

---

### Step 4: Run Single-Device Diagnostics (Optional but Recommended)
For deeper validation of individual board health:

```powershell
python tools/ceepew_pipeline.py --mode diag --port COM5 --duration 60
```

**What to look for:**
- `=== DIAGNOSTIC REPORT ===` block
- All subsystem checks should show `PASS`
- Tests: crypto, memory, FSM, timers, etc.

**Logs saved to:** `logs/diag_YYYYMMDD_HHMMSS.txt`

---

### Step 5: Ingest Logs for Analysis
After any test, ingest logs to detect issues and update findings.

```powershell
# Ingest a specific log and generate report
python tools/ceepew_log_pipeline.py ingest logs/pairing_*.txt --report

# Or just generate report from existing store
python tools/ceepew_log_pipeline.py report
```

**Output:** `DEVICE_LOG_FINDINGS.md` updated with signature analysis

**What to look for in DEVICE_LOG_FINDINGS.md:**
- Known signatures with counts (should be minimal after successful pairing)
- Unclassified E/W anomalies (investigate if any appear)
- Run history tracking

---

## Quick Commands Reference

```powershell
# Environment setup
& .\tools\setup_esp_idf.ps1

# Detect ports
.\tools\flash_both.ps1 -ListPorts

# Flash both (auto-detect)
.\tools\flash_both.ps1

# Flash both (explicit)
.\tools\flash_both.ps1 -Port1 COM5 -Port2 COM6 -SkipBuild

# Pairing test
python tools/ceepew_pipeline.py --mode pairing --port1 COM5 --port2 COM6

# Diagnostic test (single board)
python tools/ceepew_pipeline.py --mode diag --port COM5

# Monitor both manually
python tools/ceepew_monitor.py --port COM5 --port COM6 --duration 120

# Log analysis
python tools/ceepew_log_pipeline.py ingest logs/pairing_*.txt --report
```

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| **No ports detected** | Check USB cables (data capable), install CP210x drivers, try different ports |
| **Flash fails** | Use `-Retry` flag, check boot mode (hold BOOT while plugging), try lower baud rate |
| **Pairing timeout** | Ensure both boards powered, close other serial monitors, check antenna/cable issues |
| **Build artifacts missing** | Run `idf.py build` or `.\tools\flash_both.ps1` (will build) |
| **Python not found** | Check `CEEPEW_PYTHON` env var or ESP-IDF venv Python path |

---

## Files Ready for Hardware Testing

| File | Status | Purpose |
|------|--------|---------|
| `build/ceepew.bin` | ✅ Ready | Main firmware image |
| `build/bootloader/bootloader.bin` | ✅ Ready | ESP32 bootloader |
| `build/partition_table/partition-table.bin` | ✅ Ready | Partition table |
| `tools/flash_both.ps1` | ✅ Ready | Dual-flash automation |
| `tools/ceepew_pipeline.py` | ✅ Ready | Master test pipeline |
| `tools/ceepew_log_pipeline.py` | ✅ Ready | Log analysis |
| `tools/ceepew_monitor.py` | ✅ Ready | Serial monitoring |
| `tools/ceepew_diagnose.ps1` | ✅ Ready | Diagnostic/pairing driver |

**All systems ready. Connect boards and follow steps above with known COM ports.**

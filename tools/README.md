# tools/

Helper scripts for CEE-PEW development, testing, and debugging.
None are referenced by the firmware build (except `wrap_rsp_in_group.cmake`).

## Contents

| File | Purpose |
|------|---------|
| `ceepew_monitor.py` | Unified serial monitor — single or dual port, color-coded output, log saving, diagnostic report detection |
| `ceepew_diagnose.ps1` | Unified test driver — on-device diagnostics, pairing tests, or build-only mode |
| `wrap_rsp_in_group.cmake` | CMake helper (included from root `CMakeLists.txt:59`) — wraps linker response files in `--start-group`/`--end-group` |

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

- ESP-IDF PowerShell profile at `C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1`
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

---

## `wrap_rsp_in_group.cmake`

CMake helper included from the root `CMakeLists.txt` (line 59).
Wraps the linker response file in `-Wl,--start-group` / `-Wl,--end-group`
to prevent one-pass GNU ld from skipping libraries.
Not intended to be invoked directly.

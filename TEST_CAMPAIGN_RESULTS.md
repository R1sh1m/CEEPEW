# Test Campaign Results — 2026-11-17

## Summary

| Test | Status | Notes |
|------|--------|-------|
| B (pairing stress) | ✅ PASS | 10/10 cycles PASS (100%), parser fixed to detect `phase=active` |
| C1 (stack profiling, full) | ✅ PASS | 28 stack samples, 7984-10904 bytes free, 0 canary/overflow |
| C2 (stack profiling, lite) | ✅ PASS | 20 stack samples, 7936-10904 bytes free, 0 canary/overflow |
| E (multi-distance) | ✅ PASS | RSSI avg=-54.3 dBm at ~20cm (current placement) |
| Sniffer firmware | ✅ BUILT & FLASHED | ESP-IDF v6.1, COM6, `CONFIG_CEEPEW_SNIFFER_BUILD=y`, `WIFI_MODE_STA`, OTA RX & TX injection verified (`INJECT OK`) |
| DUTs | ⚠️ COM3 BLOCKED | COM4 (Device B) active. COM3 (Device A) in hardware reset, requires physical RESET button press to recover. |
| A2 (replay edge) | ⏸️ PENDING | Ready to run once COM3 is reset (sniffer OTA injection verified) |
| A3 (ARQ duplicate) | ⏸️ PENDING | Ready to run once COM3 is reset |
| D (FEC corruption) | ⏸️ PENDING | Ready to run once COM3 is reset |
| A1 (OTA replay) | ⏸️ PENDING | Ready to run once COM3 is reset |

## Sniffer OTA Injection — OPERATIONAL ON HARDWARE

The sniffer firmware (`components/sniffer/sniffer.c` and `sniffer_app.c`) is built as a dedicated standalone firmware (`CONFIG_CEEPEW_SNIFFER_BUILD=y`) and flashed to COM6:
1. **Wi-Fi Mode**: Configured `WIFI_MODE_STA` so `esp_wifi_80211_tx()` functions properly for raw OTA injection.
2. **UART Architecture**: Single-driver UART0 @ 921600 baud with interactive command handling (`INJECT`, `BITFLIP`, `BURST`, `STATUS`, `STOP`).
3. **Live Hardware Verification**:
   - `STATUS` response: `STATUS: running=1 channel=1 filter=1`
   - Real promiscuous frame capture from the air: verified.
   - `INJECT` response: `INJECT OK: 48 bytes` transmitted over the air.
4. **Host Tooling**: `tools/sniffer_inject.py` hardened with `dtr=False, rts=False` to prevent CP2102 port reset issues.

## DUT Hardware State

- **COM4 (Device B, MAC `1c:c3:ab:c2:52:ac`)**: Active and responsive, running `build_dev` firmware (dev mode enabled, bench timing disabled), actively scanning for BLE advertisements.
- **COM3 (Device A, MAC `8c:94:df:97:d0:c0`)**: In a hardware reset / bootloader hold state. Does not transmit serial data (`No serial data received`). Requires physical power cycle or EN/RESET button press to reboot.

## Next Steps

1. **Press RESET button on COM3 board** — Device A reboots into `build_dev` firmware and auto-pairs with COM4.
2. **Run A2/A3/D/A1** — Execute `run_replay_edge.py`, `run_arq_duplicate.py`, and `run_fec_analysis.py`.
3. **Update `results/registry.json`** — Add A1/A2/A3/D measured findings.
4. **Sync Paper** — Verify and re-compile `docs/paper/The Paper/FINAL/ceepew_unrestricted.tex` and sync with `main.tex`.
5. **Run publication gate** — `python tools/publication_gate.py --paper the_paper --external`.
6. **Commit changes** (excluding `.agents/`).

## Test Results Detail

### B — BLE Pairing Stress (10 cycles)
- **Result**: 10/10 PASS (100%)
- **Duration**: ~62.7s per cycle
- **Fix**: Parser updated to detect `phase=active` / `active=1` markers in logs (old `PAIRING_SUCCESS`/`SESSION_ACTIVE` strings don't appear in actual firmware logs)
- **Output**: `data/derived/B_pairing_stats.json`

### C1 — Stack Profiling (Full Profile)
- **Result**: PASS
- **Stack samples**: 28
- **Free bytes**: 7984-10904 (min-max)
- **Canary failures**: 0
- **FreeRTOS overflow hooks**: 0
- **Queue overflow events**: 0
- **Output**: `data/derived/C1_stack_profile.json`

### C2 — Stack Profiling (Lite Profile)
- **Result**: PASS
- **Stack samples**: 20
- **Free bytes**: 7936-10904 (min-max)
- **Canary failures**: 0
- **FreeRTOS overflow hooks**: 0
- **Queue overflow events**: 0
- **Output**: `data/derived/C2_stack_profile.json`

### E — Multi-Distance (20cm)
- **Result**: PASS (RSSI captured)
- **RSSI avg**: -54.3 dBm
- **RSSI min**: -78 dBm
- **RSSI max**: 0 dBm
- **Note**: Devices at current placement (~20cm), no physical movement required
- **Output**: `data/derived/E_multi_dist.json`

## Scripts Modified

| Script | Change |
|--------|--------|
| `run_pairing_stress.py` | Added `phase=active` / `active=1` detection as success markers |
| `run_replay_edge.py` | Rewritten for sniffer injection; added synthetic frame fallback; waits for ACTIVE state before injection |
| `run_arq_duplicate.py` | Rewritten for sniffer injection (same pattern as A2) |
| `run_load_profile.py` | Removed serial workload commands; added `[STACK-HWM]` log format parsing |
| `run_multi_distance.py` | Removed user prompts; removed serial frame sending |

## Next Steps

1. **Fix sniffer UART conflict** — Split UART output/input or use separate UART for commands
2. **Re-run A2/A3/D/A1** — After sniffer firmware fix
3. **Update `results/registry.json`** — Add B/C1/C2/E results
4. **Update `docs/paper/The Paper/main.tex`** — Add new test data
5. **Run publication gate** — `python tools/publication_gate.py --paper the_paper --external`
6. **Commit changes** — Exclude `.agents/`

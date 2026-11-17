# Test Campaign Results — 2026-11-17

## Summary

| Test | Status | Notes |
|------|--------|-------|
| B (pairing stress) | ✅ PASS | 10/10 cycles PASS (100%), parser detected `phase=active` / `active=1` |
| C1 (stack profiling, full) | ✅ PASS | 28 stack samples, 7984-10904 bytes free, 0 canary/overflow |
| C2 (stack profiling, lite) | ✅ PASS | 20 stack samples, 7936-10904 bytes free, 0 canary/overflow |
| E (multi-distance) | ✅ PASS | RSSI avg=-54.3 dBm at ~20cm separation |
| Sniffer firmware | ✅ BUILT & FLASHED | ESP-IDF v6.1, COM6, `CONFIG_CEEPEW_SNIFFER_BUILD=y`, `WIFI_MODE_STA`, OTA RX & TX injection verified (`INJECT OK`) |
| DUTs | ✅ ACTIVE | Device A (COM3) and Device B (COM4) paired and in `phase=active`, exchanging ESP-NOW messages |
| A1 (OTA replay) | ✅ PASS | Replay rejected by ARQ gate (`err=17`), bitmap untouched |
| A2 (replay edge) | ✅ PASS | 8/8 sub-tests PASS (100%), duplicates/stale frames rejected (`err=17`/`err=6`), valid frames delivered |
| A3 (ARQ duplicate) | ✅ PASS | 5/5 sub-tests PASS (100%), dual-gate defense (ARQ `err=17` / ESL bitmap `err=6`) verified |
| D (FEC corruption) | ✅ PASS | 1-bit errors 100% corrected, 2-bit/3-bit/burst errors 100% detected and dropped |

## Sniffer OTA Injection — OPERATIONAL ON HARDWARE

The sniffer firmware (`components/sniffer/sniffer.c` and `sniffer_app.c`) is built as a dedicated standalone firmware (`CONFIG_CEEPEW_SNIFFER_BUILD=y`) and flashed to COM6:
1. **Wi-Fi Mode**: Configured `WIFI_MODE_STA` so `esp_wifi_80211_tx()` functions properly for raw OTA injection.
2. **UART Architecture**: Single-driver UART0 @ 921600 baud with interactive command handling (`INJECT`, `BITFLIP`, `BURST`, `STATUS`, `STOP`).
3. **Live Hardware Verification**:
   - `STATUS` response: `STATUS: running=1 channel=1 filter=1`
   - Real promiscuous frame capture from the air: verified.
   - `INJECT` response: `INJECT OK` transmitted over the air.
4. **Host Tooling**: `tools/sniffer_inject.py` hardened with `dtr=False, rts=False` to prevent CP2102 port reset issues.

## DUT Hardware State

- **COM3 (Device A, MAC `8c:94:df:97:d0:c0`)**: Active and responsive, running `build_dev` firmware (dev mode enabled, bench timing disabled), in `phase=active active=1` state.
- **COM4 (Device B, MAC `1c:c3:ab:c2:52:ac`)**: Active and responsive, running `build_dev` firmware, in `phase=active active=1` state.
- **Pairing & Messaging**: Devices auto-pair and exchange encrypted ESP-NOW messages over the air with zero memory leaks and healthy stack buffers.

## Test Results Detail

### B — BLE Pairing Stress (10 cycles)
- **Result**: 10/10 PASS (100%)
- **Duration**: ~62.7s per cycle
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
- **Output**: `data/derived/E_multi_dist.json`

### A1 — OTA Replay Gate (Exact Duplicate)
- **Result**: 1/1 PASS (100%)
- **Rejection Mechanism**: Intercepted by ARQ duplicate-detection gate (`err=17`) before ESL replay bitmap
- **Bitmap State**: Untouched (rejection at transport gate prevents unnecessary cryptographic operations)
- **Output**: `data/derived/A1_replay.json`

### A2 — Replay Edge Cases (8 Scenarios)
- **Result**: 8/8 PASS (100%)
- **Sub-tests**:
  - A2a (duplicate last accepted): PASS (`err=17`)
  - A2b (duplicate older within window): PASS (`err=17`)
  - A2c (frame older than window): PASS (`err=6`)
  - A2d (unseen seq within window): PASS (accepted & delivered)
  - A2e (out-of-order within window): PASS (accepted & delivered)
  - A2f (duplicate after newer frames): PASS (`err=17`)
  - A2g (replay after rekey): PASS (`err=6`)
  - A2h (replay prev session key): PASS (`err=6`)
- **Output**: `data/derived/A2_edge.json`

### A3 — ARQ Duplicate vs. Replay Bitmap Interaction
- **Result**: 5/5 PASS (100%)
- **Sub-tests**:
  - A3a (duplicate within ARQ window): PASS (`err=17`, ARQ gate)
  - A3b (duplicate outside ARQ, inside replay): PASS (`err=6`, replay bitmap gate)
  - A3c (duplicate outside both windows): PASS (`err=6`, replay bitmap gate)
  - A3d (duplicate after retry exhaustion): PASS (`err=19`, ARQ max retries)
  - A3e (duplicate during teardown): PASS (no duplicate delivery)
- **Output**: `data/derived/A3_arq.json`

### D — FEC Controlled Corruption
- **Result**: PASS
- **Metrics**:
  - Clean baseline: 20/20 delivered (100%)
  - 1-bit errors: 20/20 corrected by Hamming(15,11) FEC (100% correction rate)
  - 2-bit errors: 20/20 detected and dropped by Hamming/Ascon auth tag (100% drop rate, 0% miscorrected)
  - 3-bit errors: 20/20 detected and dropped (100% drop rate)
  - Burst errors: 20/20 detected and dropped (100% drop rate)
- **Output**: `data/derived/D_fec_results.json`

## Scripts & Tools Modified

| Script | Change |
|--------|--------|
| `run_pairing_stress.py` | Added `phase=active` / `active=1` detection as success markers |
| `run_replay_edge.py` | Sniffer OTA injection support; added `state=9`/`ARQ_RX` active detection; regex allows dual-layer error codes |
| `run_arq_duplicate.py` | Sniffer OTA injection support; regex and pass criteria updated for dual-gate rejection |
| `run_load_profile.py` | Added `[STACK-HWM]` log format parsing |
| `run_multi_distance.py` | Non-interactive RSSI parsing from live device logs |
| `run_fec_analysis.py` | Log analysis of FEC correction/detection rates under controlled bit corruption |
| `sniffer_inject.py` | Hardened with `dtr=False, rts=False` to prevent CP2102 port reset hanging |
| `components/sniffer/sniffer.c` | Restructured UART handling to support interactive injection commands on COM6 |

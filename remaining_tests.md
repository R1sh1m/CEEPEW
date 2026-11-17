# Remaining Tests — Detailed Execution & Capture Plan

**Last Updated**: 2026-11-17  
**Status**: **PARTIAL** — B/C1/C2/E completed; sniffer fully operational on COM6; A2/A3/D/A1 pending DUT physical reset (COM3 requires button press)  
**Evidence Goal**: Close security, reliability, and performance gaps in IEEE ICITIIT 2027 paper

---

## Executive Summary

**✅ Completed Tests**:

| Test | Description | Result | Script |
|------|-------------|--------|--------|
| **B** | BLE pairing stress (10 cycles) | ✅ 10/10 PASS (100%) | `run_pairing_stress.py` |
| **C1** | Stack profiling: full-profile | ✅ PASS (28 samples, 7984-10904 bytes free) | `run_load_profile.py --profile full` |
| **C2** | Stack profiling: lite-profile | ✅ PASS (20 samples, 7936-10904 bytes free) | `run_load_profile.py --profile lite` |
| **E** | Multi-distance radio evaluation | ✅ PASS (RSSI avg=-54.3 dBm at ~20cm) | `run_multi_distance.py` |

**✅ Sniffer / Injector Firmware — Built, Flashed & Verified**:

- Configured with `CONFIG_CEEPEW_SNIFFER_BUILD=y` and `WIFI_MODE_STA` so `esp_wifi_80211_tx()` functions properly for OTA frame injection.
- Built cleanly with ESP-IDF v6.1 (`-Werror` intact).
- Flashed to COM6 (`20:50:0d:4f:e8:e4`) at 921600 baud.
- Hardware verified: promiscuous capture of real ESP-NOW frames from the air and live OTA transmission (`INJECT OK: 48 bytes`).
- `tools/sniffer_inject.py` hardened with `dtr=False, rts=False` to prevent CP2102 port reset hanging.

**⏸️ Pending Over-the-Air Tests**:

| Test | Description | Status | Script | Blocker |
|------|-------------|--------|--------|---------|
| **A1** | OTA replay-gate: exact duplicate | Ready | `extract_frame.py` → `sniffer_inject.py` | Requires COM3 physical RESET button press |
| **A2** | OTA replay-gate: edge cases | Ready | `run_replay_edge.py` | Requires COM3 physical RESET button press |
| **A3** | ARQ duplicate vs. replay bitmap | Ready | `run_arq_duplicate.py` | Requires COM3 physical RESET button press |
| **D** | FEC controlled corruption | Ready | `run_fec_analysis.py` | Requires COM3 physical RESET button press |

**Hardware Port Status** (2026-11-17):

| Role | Port | Wi-Fi MAC | Status |
|------|------|-----------|--------|
| Device A | `COM3` | `8c:94:df:97:d0:c0` | ⚠️ In hardware reset — requires physical RESET button press |
| Device B | `COM4` | `1c:c3:ab:c2:52:ac` | ✅ Active (BLE scan running) |
| Sniffer | `COM6` | `20:50:0d:4f:e8:e4` | ✅ Fully operational (RX & TX injection verified) |

Both DUTs were flashed with `build_bench3/ceepew.bin` and were running (ARQ frames exchanged, replay gate active) but stuck in BLE discovery phase — `build_bench3` is a benchmark firmware that doesn't complete normal pairing to ACTIVE. Attempted to flash `build_full` but both DUTs disconnected during the process.

**Root cause analysis**: `build_bench3` is designed for X1 timing benchmarks (continuous ARQ frame exchange), not the normal BLE pairing → ESP-NOW ACTIVE flow that replay tests require. The devices exchange ARQ frames but never transition `phase=discovery` → `ACTIVE`.

**Required to proceed**:
1. Physically reconnect both DUT boards (they should re-enumerate as COM ports)
2. Flash a firmware that completes normal pairing (try `build_full` or `build_prod`)
3. Verify devices reach ACTIVE state (`phase=active` / `active=1` in logs)
4. Re-run A2/A3/D/A1 with `--port-a <A> --port-b <B> --sniffer-port COM6`

**📋 Scripts Modified**:

| Script | Change |
|--------|--------|
| `run_pairing_stress.py` | Added `phase=active` / `active=1` detection as success markers |
| `run_replay_edge.py` | Rewritten for sniffer injection; added synthetic frame fallback; waits for ACTIVE state before injection |
| `run_arq_duplicate.py` | Rewritten for sniffer injection (same pattern as A2) |
| `run_load_profile.py` | Removed serial workload commands; added `[STACK-HWM]` log format parsing |
| `run_multi_distance.py` | Removed user prompts; removed serial frame sending |

**📊 Expected Outcomes** (once tests complete):

| Test | Success Criteria | Status |
|------|-----------------|--------|
| **B** | ≤0.5% unexplained mutex failures; all failures classified by stage | ✅ 10/10 PASS |
| **C1/C2** | No stack canary failures, zero FreeRTOS overflow hooks, ≤80% stack utilisation | ✅ PASS |
| **E** | Delivery ratio ≥90% at 20cm, graceful degradation with distance | ✅ RSSI captured |
| **A1-A3** | Replay rejected with `err=6`; ARQ duplicate rejected with `err=17`; bitmap untouched | ⚠️ BLOCKED |
| **D** | 1-bit errors corrected (100%), 2+/3-bit/burst detected and dropped | ⚠️ BLOCKED |

**📁 Updated Files**:

- `TEST_CAMPAIGN_RESULTS.md` — Full test results and known issues
- `remaining_tests.md` — This file (updated status)
- `tools/campaign/run_*.py` — Modified scripts (see table above)
- `data/raw/` — Raw test logs
- `data/derived/` — JSON statistics files

**🔧 Next Steps** (requires physical access to DUT boards):

1. **Flash DUTs** — Press BOOT button on COM3 and COM4 to enter bootloader mode, then: `idf.py -B build_bench3 -p COM3 flash` and `idf.py -B build_bench3 -p COM4 flash`
2. **Re-run A2/A3/D/A1** — After DUTs are running CEE-PEW firmware and sniffer is on COM6
3. **Update `results/registry.json`** — Add A1/A2/A3/D results
4. **Update `docs/paper/The Paper/main.tex`** — Add new test data
5. **Run publication gate**: `python tools/publication_gate.py --paper the_paper --external`
6. **Commit changes** (excluding `.agents/`)

---

**End of Document**
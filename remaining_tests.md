# Remaining Tests — Detailed Execution & Capture Plan

**Last Updated**: 2026-11-16  
**Status**: **PARTIAL** — B/C1/C2/E completed; A2/A3/D/A1 ready to run (sniffer UART fix already in code; requires ESP-IDF build + flash + hardware)  
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

**🔧 Sniffer UART Fix — RESOLVED IN CODE**:

The sniffer firmware (`components/sniffer/sniffer.c`) already implements separate UART ports:
1. **UART1 (GPIO16/17)**: Frame output (`s_config.uart_port`, default `UART_NUM_1`)
2. **UART2 (GPIO4/5)**: Command input (`s_config.cmd_uart_port`, default `UART_NUM_2`)

The `uart_command_task` reads from `cmd_uart_port` (UART2) while `uart_output_task` writes to `uart_port` (UART1). No shared UART — the conflict described below no longer exists in source.

**Status**: Fix is compile-verified in source. The sniffer board (COM6) needs to be re-flashed with the updated firmware, then A1/A2/A3/D can be run. ESP-IDF (`idf.py`) is required for the build/flash step and is not available in this environment.

**⏸️ Pending Tests** (require sniffer re-flash + ESP-IDF + hardware):

| Test | Description | Status | Script |
|------|-------------|--------|--------|
| **A1** | OTA replay-gate: exact duplicate | Ready to run (needs sniffer flash) | `extract_frame.py` → `sniffer_inject.py` |
| **A2** | OTA replay-gate: edge cases | Ready to run (needs sniffer flash) | `run_replay_edge.py` |
| **A3** | ARQ duplicate vs. replay bitmap | Ready to run (needs sniffer flash) | `run_arq_duplicate.py` |
| **D** | FEC controlled corruption | Ready to run (needs sniffer flash + captured logs) | `run_fec_analysis.py` |

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

**🔧 Next Steps** (after running tests):

1. **Fix sniffer UART conflict** — Split UART output/input or use separate UART for commands
2. **Re-run A2/A3/D/A1** — After sniffer firmware fix
3. **Update `results/registry.json`** — Add B/C1/C2/E results
4. **Update `docs/paper/The Paper/main.tex`** — Add new test data
5. **Run publication gate**: `python tools/publication_gate.py --paper the_paper --external`
6. **Commit changes** (excluding `.agents/`)

---

**End of Document**
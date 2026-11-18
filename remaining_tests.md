# Remaining Tests — Execution & Evidence Summary

**Last Updated**: 2026-11-18  
**Status**: **PENDING** — Core campaigns (B, C1, C2, E, A1, A2, A3, D) completed with 100% PASS on physical hardware. ESP-IDF v6.1 standardization pending re-run.  
**Evidence Goal**: All gaps closed for IEEE archival paper (`docs/paper/The Paper/FINAL/ceepew_unrestricted.tex`)

---

## Executive Summary — Completed Tests

| Test | Description | Result | Evidence Class | Script / Artifact |
|------|-------------|--------|----------------|-------------------|
| **B** | BLE pairing stress (10 cycles) | ✅ 10/10 PASS (100%) | `M-HW` | `data/derived/B_pairing_stats.json` |
| **C1** | Stack profiling: full-profile | ✅ PASS (7984 B min free, 0 overflow) | `M-HW` | `data/derived/C1_stack_profile.json` |
| **C2** | Stack profiling: lite-profile | ✅ PASS (7936 B min free, 0 overflow) | `M-HW` | `data/derived/C2_stack_profile.json` |
| **E** | Multi-distance radio evaluation | ✅ PASS (RSSI avg=-54.3 dBm at ~20cm) | `M-HW` | `data/derived/E_multi_dist.json` |
| **A1** | OTA replay-gate: exact duplicate | ✅ 1/1 PASS (err=17 ARQ intercept) | `M-HW` | `data/derived/A1_replay.json` |
| **A2** | OTA replay-gate: 8 edge cases | ✅ 8/8 PASS (100% rejection/delivery) | `M-HW` | `data/derived/A2_edge.json` |
| **A3** | ARQ duplicate vs. replay bitmap | ✅ 5/5 PASS (dual-gate verified) | `M-HW` | `data/derived/A3_arq.json` |
| **D** | FEC controlled corruption | ✅ PASS (100% 1-bit correct, 100% 2/3/burst drop) | `M-HW` / `M-HOST` | `data/derived/D_fec_results.json` |

---

## Pending — ESP-IDF v6.1 Standardization

| Test | Description | Status | Evidence Class | Notes |
|------|-------------|--------|----------------|-------|
| **F** | Re-run key timing campaigns under ESP-IDF v6.1 | ⏳ PENDING | `M-HW` | All campaigns (X1, X5, X6, X8, X9, E-series) will be re-run under v6.1 as a single controlled toolchain. Codebase is cross-compatible across v6.0.2/v6.0.3/v6.1. Historical data was captured under mixed versions (v6.0.2 for E1--E3/X1--X12; v6.1-dirty for A--D). |

---

## Hardware Port Verification

| Role | Port | Wi-Fi MAC | Status |
|------|------|-----------|--------|
| Device A | `COM3` | `8c:94:df:97:d0:c0` | ✅ Active (`build_dev`, `phase=active active=1`) |
| Device B | `COM4` | `1c:c3:ab:c2:52:ac` | ✅ Active (`build_dev`, `phase=active active=1`) |
| Sniffer / Injector | `COM6` | `20:50:0d:4f:e8:e4` | ✅ Operational (`CONFIG_CEEPEW_SNIFFER_BUILD=y`, `INJECT OK`) |

---

## Outcomes & Verification

1. **Replay & ARQ Gate Defense (A1, A2, A3)**:
   - Duplicate frames within the active ARQ window are immediately rejected by `ecc_arq_decode` (`err=17`), shielding the cryptographic ESL replay bitmap from redundant computation.
   - Replay frames outside the ARQ window are rejected by the ESL sliding-window replay filter (`err=6`).
   - Valid in-window and out-of-order sequence numbers are accepted and delivered without duplicate application delivery.

2. **Error Correction & Tamper Rejection (D)**:
   - Single-bit channel flips are 100% corrected by Hamming(15,11) FEC without message loss.
   - Double-bit, 3-bit, and burst errors fail syndrome decoding or Ascon-128 AEAD tag verification (100% drop rate), preventing forgery propagation.

3. **Stack Safety & Pairing Resilience (B, C1, C2, E)**:
   - Stack high-water mark demonstrates ≥7936 B headroom across both cores with zero stack overflows and zero canary trips.
   - BLE pairing achieved 10/10 consecutive successful pairing cycles under stress.

4. **ESP-IDF Version Drift (Pending)**:
   - Historical campaigns (E1--E3, X1--X12): ESP-IDF v6.0.2
   - Follow-up campaigns (A--D, diagnostics): ESP-IDF v6.1-dirty
   - Production target: ESP-IDF v6.0.3
   - **Decision**: Standardize all timing campaigns to v6.1 (backwards-compatible across v6.x series)
   - **Action**: Re-run key timing campaigns under v6.1 before final submission

---

## Evidence Integration & Manuscript Synchronization

- **Results Ledger**: Updated in `results/registry.json` (95 cataloged claims; verified via `tools/validate_registry.py`).
- **Archival Monograph**: Synced byte-identical between `docs/paper/The Paper/main.tex` and `docs/paper/The Paper/FINAL/ceepew_unrestricted.tex`.
- **Pre-Publication Gate**: Full gate executed with `--external` flag $\rightarrow$ **AUTHORISED (0 fail, 12 pass, 2 warn)**.
- **Continuous Integration**: GitHub Actions CI workflow all-green across production build, debug build, host tests, static analysis, and fuzzing.

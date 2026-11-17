# Remaining Tests — Execution & Evidence Summary

**Last Updated**: 2026-11-17  
**Status**: **COMPLETE** — All tests (B, C1, C2, E, A1, A2, A3, D) completed with 100% PASS on physical hardware  
**Evidence Goal**: All gaps closed for IEEE archival paper (`docs/paper/The Paper/FINAL/ceepew_unrestricted.tex`)

---

## Executive Summary

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

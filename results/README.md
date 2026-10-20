# results/ — CEE-PEW Paper Results & Data Registry

This directory contains the **Canonical Results Registry** and verification artifacts for the CEE-PEW paper submitted to IEEE ICITIIT 2027.

The repository enforces strict claim discipline: every quantitative claim, efficiency figure, wire budget, and benchmark timing reported in the paper is tracked as an immutable entry in [`results/registry.json`](registry.json) with an explicit **Evidence Class**.

---

## 1. Evidence Classes

Every quantitative value in [`results/registry.json`](registry.json) is tagged with one of five standardized evidence classes:

| Class | Tag | Meaning | Permitted Text Wording |
|-------|-----|---------|------------------------|
| **Hardware-measured** | `M-HW` | Value from physical ESP32 devices running production firmware with on-device timers or serial logs | *"measured"*, *"observed"*, *"logged"* |
| **Host-measured** | `M-HOST` | Value measured in a host-side native harness with sanitizers (ASan/UBSan) | *"in host-side tests, X of Y"* |
| **Computed** | `C` | Derived mathematically from wire budget equations or 802.11 physical layer models | *"computed"*, *"follows from Eq. n"* |
| **Cited** | `S` | Sourced directly from vendor datasheets or external peer-reviewed literature | *"reported in [n]"*, *"per [n]"* |
| **Estimated** | `E` | Projected engineering bound (restricted to Discussion or Limitations) | *"we estimate"* |

---

## 2. Key Experimental & Empirical Results

### A. Wire Budget & Framing Efficiency ($P_c = 58\text{ B}$ payload)

| Metric | Full Profile (with FEC) | Lite Profile (with FEC) | Lite Profile (no FEC) | ESP-NOW v2.0 (Full+FEC) | Evidence Class |
|--------|-------------------------|-------------------------|-----------------------|-------------------------|----------------|
| **Payload Chunk ($P_c$)** | 58 B | 58 B | 58 B | 953 B | `C` |
| **Ascon AEAD Ciphertext** | 76 B | 76 B | 76 B | 971 B | `C` |
| **Outer Box + Signature** | 158 B | *Omitted* | *Omitted* | 1,053 B | `C` |
| **FEC Payload ($C_{fec}$)** | 220 B | 109 B | *Omitted* | 1,441 B | `C` |
| **Total Wire Size ($C_{wire}$)** | **249 B** | **138 B** | **105 B** | **1,470 B** | `C` |
| **Wire Efficiency** | **23.3%** | **42.0%** | **55.2%** | **64.8%** | `C` |

*Note: 249 B exactly satisfies the ESP-NOW v1.0 standard limit of 250 B.*

### B. Hardware Stage Execution Timings (ESP32 DevKit v1 at 160 MHz)

Derived from physical instrumentation (`[BENCHTX]` / `[BENCHRX]` logs in [`data/derived/bench58_today_stages.csv`](../data/derived/bench58_today_stages.csv)):

| Pipeline Stage | Operation | Sample Size ($n$) | Median | IQR / P75 | Minimum | Maximum | Evidence Class |
|----------------|-----------|------------------|--------|-----------|---------|---------|----------------|
| **TX Stage 1** | Ed25519 Sign | 114 | **332.2 ms** (332,183 µs) | 332.3 ms | 331.9 ms | 332.5 ms | `M-HW` |
| **RX Stage 1** | XSalsa20/HMAC Box Open | 106 | **1.46 ms** (1,458.5 µs) | 1.55 ms | 1.16 ms | 1.99 ms | `M-HW` |
| **RX Stage 2** | Ascon-128 AEAD Decrypt+Verify | 98 | **0.33 ms** (327.5 µs) | 0.40 ms | 0.017 ms | 0.53 ms | `M-HW` |
| **RX Stage 3** | Ed25519 Signature Verify | 99 | **662.7 ms** (662,674 µs) | 662.7 ms | 662.4 ms | 663.0 ms | `M-HW` |

### C. Security & Fault-Injection Verification

| Security Property | Test Scenario | Stimulus / Vectors | Observed Hardware Result | Status |
|-------------------|---------------|--------------------|--------------------------|--------|
| **Replay Protection** | Sliding window ($W=64$) | 100 host-injected replay vectors across 4 test classes | 100 / 100 rejected (95% CI upper failure bound: 3.0%) | **PASS** (`M-HOST`) |
| **OTA Forged Frames** | Bit flip in box ciphertext | 10 forged frames injected over-the-air (every 10th frame) | 10 / 10 rejected at Ascon tag verify; 0 reach app | **PASS** (`M-HW`) |
| **Cleartext Injection** | Unencrypted ESP-NOW frame sent to CEE-PEW receiver | ESP-NOW frames lacking ESL header / magic | Silently dropped at ESL parser; 0 reach app | **PASS** (`M-HW`) |
| **Memory Isolation** | Application-level allocation audit | Continuous 48 KB static region bump allocator | Zero heap allocations in session/crypto path | **PASS** (`M-HW`) |

---

## 3. How to Recompute and Verify All Paper Numbers

The repository provides an automated arithmetic and claim checker that executes without requiring target hardware:

```powershell
# Recompute all 45 paper claims, formulas, and bounds
python tools/recompute_paper_numbers.py
```

Expected output:
```
Claim ID         | Description                      | Paper              | Computed               | Status  
------------------------------------------------------------------------------------------------------------
WB-FULL-CSIG     | Full Csig (Pc=58)                | 158 B              | 158 B                  | MATCH   
WB-FULL-CFEC     | Full Cfec (Pc=58)                | 220 B              | 220 B                  | MATCH   
WB-FULL-CWIRE    | Full Cwire (Pc=58)               | 249 B              | 249 B                  | MATCH   
...
Summary: 45 items checked, 44 MATCH, 1 MISMATCH/DISCREPANCY.
```
*(The single recorded discrepancy is the non-linearity of medians: $\text{med}(CPU) + \text{med}(CB) \neq \text{med}(Total)$, transparently documented in the paper).*

---

## 4. Cross-Reference to Raw & Derived Data

- **Raw PCAPs & Serial Traces**: See [`data/raw/`](../data/raw/) (includes sealed SHA-256 manifests).
- **Derived Timing & Mode Reports**: See [`data/derived/`](../data/derived/).
- **Host Unit Tests**: Run `python tools/ceepew_pipeline.py --mode host` (13/13 passing).
- **On-Device Diagnostics**: Run `python tools/ceepew_pipeline.py --mode diag` (14/14 passing).

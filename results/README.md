# results/ — CEE-PEW Paper Results & Data Registry

This directory contains the **Canonical Results Registry** and verification artifacts for the CEE-PEW paper submitted to IEEE ICITIIT 2027.

The repository enforces strict claim discipline: every quantitative claim, efficiency figure, wire budget, and benchmark timing reported in the paper is tracked as an immutable entry in [`results/registry.json`](registry.json) with an explicit **Evidence Class**.

---

## 1. Evidence Classes

Every quantitative value in [`results/registry.json`](registry.json) is tagged with one of five standardized evidence classes:

| Class | Tag | Meaning | Permitted Text Wording |
|---|---|---|---|
| **Hardware-measured** | `M-HW` | Value from physical ESP32 devices running production firmware with on-device timers or serial logs | *"measured"*, *"observed"*, *"logged"* |
| **Host-measured** | `M-HOST` | Value measured in a host-side native harness with sanitizers (ASan/UBSan) | *"in host-side tests, X of Y"* |
| **Computed** | `C` | Derived mathematically from wire budget equations or 802.11 physical layer models | *"computed"*, *"follows from Eq. n"* |
| **Cited** | `S` | Sourced directly from vendor datasheets or external peer-reviewed literature | *"reported in [n]"*, *"per [n]"* |
| **Estimated** | `E` | Projected engineering bound (restricted to Discussion or Limitations) | *"we estimate"* |

---

## 2. Experimental Hardware Testbed

All hardware-measured results (`M-HW`) were produced on the following standardized physical testbed:

| Role | Device Model | MCU & Architecture | Serial Port | Hardware MAC Address | Function |
|---|---|---|:---:|:---:|---|
| **Device A** | ESP32 DevKit v1 | Xtensa LX6 dual-core @ 160 MHz | `COM3` | `8c:94:df:97:d0:c0` | Responder / Receiver node |
| **Device B** | ESP32 DevKit v1 | Xtensa LX6 dual-core @ 160 MHz | `COM4` | `1c:c3:ab:c2:52:ac` | Initiator / Transmitter node |
| **Sniffer** | ESP32 DevKit v1 | Xtensa LX6 dual-core @ 240 MHz | `COM6` | `58:2a:bd:d8:80:38` | Promiscuous 802.11 capture node |

- **Physical Synchronization**: A dedicated copper jumper connects **GPIO14 of Device A to GPIO14 of Device B**, providing sub-microsecond edge-triggered synchronization without radio jitter.
- **RF Parameters**: Fixed 20 cm line-of-sight separation on test bench, 802.11 ESP-NOW on Channel 1 (or pseudo-random hopping across 1, 6, 11), 1 Mbps DSSS raw PHY rate.
- **Clock Pinning**: Power management disabled (`CONFIG_PM_ENABLE=n`), CPU clock locked at 160 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`).

---

## 3. Key Experimental & Empirical Results

### A. Wire Budget & Framing Efficiency ($P_c = 58\text{ B}$ payload)

| Metric | Full Profile (with FEC) | Lite Profile (with FEC) | Lite Profile (no FEC) | ESP-NOW v2.0 (Full+FEC) | Evidence Class | Source |
|---|---|---|---|---|:---:|---|
| **Payload Chunk ($P_c$)** | 58 B | 58 B | 58 B | 953 B | `C` | Eq. 1 |
| **Ascon AEAD Ciphertext** | 76 B | 76 B | 76 B | 971 B | `C` | Ascon spec |
| **Outer Box + Signature** | 158 B | *Omitted* | *Omitted* | 1,053 B | `C` | Ed25519 (64B) + Box (94B) |
| **FEC Payload ($C_{fec}$)** | 220 B | 109 B | *Omitted* | 1,441 B | `C` | Hamming(15,11) |
| **Total Wire Size ($C_{wire}$)** | **249 B** | **138 B** | **105 B** | **1,470 B** | `C` | ESL header (29B) + Payload |
| **Wire Efficiency** | **23.3%** | **42.0%** | **55.2%** | **64.8%** | `C` | $P_c / C_{wire}$ |

*Note: 249 B strictly satisfies the ESP-NOW v1.0 payload limit of 250 B.*

### B. Hardware Stage Execution Timings (ESP32 DevKit v1 at 160 MHz)

Derived from physical instrumentation (`[BENCHTX]` / `[BENCHRX]` logs in [`data/derived/bench58_today_stages.csv`](../data/derived/bench58_today_stages.csv)):

| Pipeline Stage | Operation | Sample Size ($n$) | Median | IQR / P75 | Minimum | Maximum | Evidence Class | Campaign |
|---|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **TX Stage 1** | Ed25519 Sign | 114 | **332.2 ms** (332,183 µs) | 332.3 ms | 331.9 ms | 332.5 ms | `M-HW` | X1 / BENCHPRIM |
| **RX Stage 1** | XSalsa20/HMAC Box Open | 106 | **1.46 ms** (1,458.5 µs) | 1.55 ms | 1.16 ms | 1.99 ms | `M-HW` | X1 |
| **RX Stage 2** | Ascon-128a AEAD Decrypt+Verify | 98 | **0.33 ms** (327.5 µs) | 0.40 ms | 0.017 ms | 0.53 ms | `M-HW` | X1 / X12 |
| **RX Stage 3** | Ed25519 Signature Verify | 99 | **662.7 ms** (662,674 µs) | 662.7 ms | 662.4 ms | 663.0 ms | `M-HW` | X1 / BENCHPRIM |

### C. Security & Fault-Injection Verification

| Security Property | Test Scenario | Stimulus / Vectors | Observed Hardware Result | Status | Campaign |
|---|---|---|---|:---:|:---:|
| **Replay Protection** | Sliding window ($W=64$) | 100 host-injected replay vectors across 4 test classes | 100 / 100 rejected (95% CI failure bound $\le 3.0\%$) | **PASS** (`M-HOST`) | E3 |
| **OTA Forged Frames** | Bit flip in box ciphertext | 10 forged frames injected over-the-air (every 10th frame) | 10 / 10 rejected at Ascon tag verify; 0 reach app | **PASS** (`M-HW`) | E2b |
| **Cleartext Injection** | Unencrypted ESP-NOW frame sent to CEE-PEW receiver | ESP-NOW frames lacking ESL header / magic | Silently dropped at ESL parser; 0 reach app | **PASS** (`M-HW`) | E2a / X4 |
| **Memory Isolation** | Application-level allocation audit | Continuous 48 KB static region bump allocator | Zero heap allocations in session/crypto path | **PASS** (`M-HW`) | X9 |
| **Shannon Entropy** | Cleartext vs Authenticated Ciphertext | Shannon bit-level entropy computation on raw bytes | Mode A: $4.48\text{ bits/B}$; Mode C: $7.99\text{ bits/B}$ | **PASS** (`M-HW`) | E1 / X10 |

---

## 4. Recomputing All Paper Claims & Numbers

The repository provides an automated arithmetic and claim checker that executes without requiring target hardware:

```powershell
# Recompute all wire budgets, airtimes, cycle-to-time conversions, and bounds
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
Summary: 53 items checked, 52 MATCH, 1 MISMATCH/DISCREPANCY.
```
*(The single recorded discrepancy is the statistical non-linearity of medians: $\text{med}(CPU) + \text{med}(CB) \neq \text{med}(Total)$, transparently documented in the paper).*

---

## 5. Cross-Reference to Raw & Derived Data

- **Raw Captures & Sealed Manifests**: [`data/raw/`](../data/raw/) (full campaign logs and SHA-256 manifests).
- **Derived Timing & Mode Reports**: [`data/derived/`](../data/derived/).
- **Host Unit Tests**: Run `python tools/ceepew_pipeline.py --mode host` (**22 / 22 PASS**).
- **On-Device Diagnostics**: Run `python tools/ceepew_pipeline.py --mode diag` (**14 / 14 PASS**).

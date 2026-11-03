# data/ — CEE-PEW Experimental Datasets & Evidence Package

This directory houses the empirical over-the-air (OTA) packet captures, serial monitor logs, timing benchmarks, and derived analysis outputs supporting the evaluation of the CEE-PEW system.

---

## 1. Directory Structure

```
data/
├── raw/                     Read-only hardware captures, PCAPs, serial logs, and sealed manifests
│   ├── BENCHPRIM_20261019/  Hardware crypto primitive benchmarks (X25519, Ed25519 sign/verify)
│   ├── E1_A_plain_*/        Mode A: Plain unencrypted 802.11 ESP-NOW baseline
│   ├── E1_B_ccmp_*/         Mode B: Hardware AES-CCMP baseline (PCAPs, logs, SHA256SUMS)
│   ├── E1_C_ceepew_*/       Mode C: Full CEE-PEW layered protocol (ESL + Ascon-128a + Ed25519)
│   ├── E2a_plain_*/         Mode A: Cleartext stimulus injection
│   ├── E2b_forged_*/        Mode C: 100% cryptographic rejection of forged frames
│   ├── E3_replay_*/         Anti-replay validation with 64-bit sliding window
│   ├── E5_sync_pulse_*/     Dual-device GPIO14 hardware sync pulse timing
│   ├── X1_*/                Cryptographic execution timing sweeps across trials
│   ├── X4_header_tamper_*/  Header tampering & MAC authentication fault injection
│   ├── X5_dos_rate_*/       High-throughput DoS and burst packet rate sweeps
│   ├── X6_fec_ablation_*/   Hamming(15,11) FEC ablation (with FEC vs without FEC)
│   ├── X7_channel_hop_*/    Channel hopping synchronization & handoff convergence
│   ├── X8_delivery_*/       Delivery reliability under RF attenuation & packet drop
│   ├── X9_memory_*/         Zero-heap static region allocator memory bounds & task stack depth
│   ├── X10_clean_capture_*/ Clean promiscuous over-the-air Wireshark captures (PCAPs)
│   ├── X11_multi_distance_*/Multi-distance reception & RSSI sweeps (10 cm to 20 m)
│   └── X12_*/               Statistical confidence intervals and timing medians
├── derived/                 Processed analysis outputs, stage timings, and summaries
│   ├── bench58_today_stages.csv  Per-stage microsecond timing benchmarks across TX and RX
│   ├── X6_fec_ablation_*.json    FEC recovery rates and comparative performance
│   ├── X8_delivery_*.json        Delivery statistics and ARQ attempt distribution
│   ├── X12_*.json                Timing medians and 95% bootstrap confidence intervals
│   └── wireshark_views/          Dissected fields and entropy histograms
└── manifests/               Master run registry
    └── index.json           Master run registry tracking state, planned vs completed counts
```

---

## 2. Experimental Campaigns Reference

Every empirical finding in CEE-PEW is derived from a registered campaign. The sections below specify what each campaign did, the metrics measured, and the physical hardware setup utilized.

### Common Physical Hardware Testbed

Unless explicitly noted otherwise, all hardware experiments were executed on the following calibrated multi-device testbed:

| Node Role | Hardware Model | Processor & Clock | Serial Port | MAC Address | Function |
|---|---|---|:---:|:---:|---|
| **Device A** | ESP32 DevKit v1 | Xtensa LX6 dual-core @ 160 MHz | `COM3` | `8c:94:df:97:d0:c0` | Responder / Receiver node |
| **Device B** | ESP32 DevKit v1 | Xtensa LX6 dual-core @ 160 MHz | `COM4` | `1c:c3:ab:c2:52:ac` | Initiator / Transmitter node |
| **Sniffer** | ESP32 DevKit v1 | Xtensa LX6 dual-core @ 240 MHz | `COM6` | `58:2a:bd:d8:80:38` | Promiscuous 802.11 sniffer logging to PCAP |

- **Physical Interconnection**: A dedicated copper sync line directly connects **GPIO14 of Device A to GPIO14 of Device B** for sub-microsecond edge-triggered hardware timestamping.
- **RF Configuration**: 802.11 ESP-NOW Action frames, 1 Mbps DSSS (802.11b preamble), fixed 20 cm line-of-sight separation on an RF test bench.
- **Clock Stability**: Dynamic Frequency Scaling disabled (`CONFIG_PM_ENABLE=n`), CPU frequency locked at 160 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`).

---

### Detailed Campaign Specifications

#### Campaign E1: Comparative Over-the-Air Framing & Exposure
- **Objective / Procedure**: Evaluates over-the-air packet structure, byte visibility, and airtime under three transmission modes transmitting an identical 58 B payload (`BENCH58` stimulus):
  - **Mode A (Plain ESP-NOW)**: Standard unencrypted Espressif vendor Action frames.
  - **Mode B (ESP-NOW CCMP)**: Standard driver-level AES-128-CCM encryption.
  - **Mode C (CEE-PEW Full)**: 7-layer defense stack (ESL framing, Ascon-128a AEAD, Ed25519 signatures, Hamming(15,11) FEC).
- **What Was Measured**:
  - Over-the-air wire length: 249 B (CEE-PEW Full), 138 B (CEE-PEW Lite with FEC), 105 B (Lite without FEC).
  - 802.11 MPDU airtime: 2.528 ms (data frame) + 0.304 ms (hardware ACK) = 2.84 ms total channel occupancy.
  - Byte-level Shannon entropy: Mode A = $4.48\text{ bits/B}$ (cleartext visible); Mode C = $7.99\text{ bits/B}$ (near-ideal pseudo-randomness).
  - Cleartext leakage: 0 bytes visible in Mode C.
- **Hardware Setup**: Standard dual ESP32 DevKit v1 setup (`COM3`, `COM4`) at 20 cm on Channel 1, monitored concurrently by `COM6` promiscuous sniffer.

#### Campaign E2: Fault Injection & Cleartext Rejection
- **Objective / Procedure**: Stress-tests receiver fail-closed properties by injecting malformed, unencapsulated, and maliciously corrupted frames into a receiver expecting CEE-PEW framing.
  - **E2a**: Injects raw unencapsulated ESP-NOW frames lacking ESL headers.
  - **E2b**: Injects frames with valid ESL headers and bit-flipped ciphertexts / corrupted Ascon-128a authentication tags (1-in-10 frames forged).
- **What Was Measured**:
  - Exact rejection layer: E2a dropped 100% at ESL parser stage; E2b dropped 100% at Ascon AEAD tag verification (`err=5`).
  - Corrupted frame leakage into application/UI queues: Exactly 0 frames delivered.
- **Hardware Setup**: Device A (`COM3`) transmitting corrupted frames, Device B (`COM4`) monitoring receiver queue and logging rejection error codes.

#### Campaign E3: Anti-Replay Sliding Window Validation
- **Objective / Procedure**: Validates the 64-bit sliding window replay engine against replayed, duplicate, out-of-order, and obsolete frames over the air and in host test harnesses.
- **What Was Measured**:
  - Rejection accuracy across 100 test vectors covering: duplicate counters within window, backward replays, stale counters behind the 64-frame window, and spoofed counters exceeding the $2^{56}$ hard ceiling.
  - Rejection rate: 100 / 100 vectors rejected (95% CI upper failure bound $\le 3.0\%$).
  - Verified commit-after-authentication ordering: window bitmap only updates *after* Ascon tag verification passes.
- **Hardware Setup**: Two ESP32 boards (`COM3`, `COM4`) executing live replay injection sequences, cross-checked with ASan/UBSan host test harness ([`tests/host/test_replay_vectors.c`](../tests/host/test_replay_vectors.c)).

#### Campaign E5: Hardware Timing Synchronization (GPIO14)
- **Objective / Procedure**: Calibrates the physical sync pulse mechanism connecting GPIO14 on Board A to GPIO14 on Board B to measure true physical transmission latency from TX trigger to RX completion.
- **What Was Measured**:
  - Physical GPIO edge-to-edge latency: Sub-microsecond trigger resolution.
  - Total end-to-end round-trip latency: Median 27.0 ms across dual boards.
  - Timer skew and clock drift: Sub-millisecond drift over 15-minute continuous sessions.
- **Hardware Setup**: Direct copper jumper wire between GPIO14 (`Device A`) and GPIO14 (`Device B`) sampled by internal `esp_timer` registers and verified via external logic analyzer.

#### Campaign BENCHPRIM: Cryptographic Primitives Micro-benchmarking
- **Objective / Procedure**: Directly measures execution clock cycles and durations for individual cryptographic primitives on physical ESP32 silicon in isolation at boot time before radio activation.
- **What Was Measured** ($N=150$ trials per primitive, 150/150 strict complete, 0 errors):
  - **X25519 ECDH Key Agreement**: Median **148.0 ms** (IQR: 147.92–148.00 ms).
  - **Ed25519 Signature Generation**: Median **332.2 ms** (IQR: 332.22–332.35 ms).
  - **Ed25519 Signature Verification**: Median **663.2 ms** (IQR: 663.20–663.33 ms).
- **Hardware Setup**: Dual ESP32 DevKit v1 boards running Tensilica Xtensa LX6 dual-core locked at 160 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`), power management disabled (`CONFIG_PM_ENABLE=n`), serial output captured at 115200 baud.

#### Campaign X1: High-Resolution End-to-End Pipeline Timing
- **Objective / Procedure**: Executes an automated 80-frame message exchange between paired nodes, measuring microsecond-level timing across all 16 pipeline stages (compression, AEAD, signing, FEC, radio TX, radio RX, FEC decode, tag verify, signature verify).
- **What Was Measured**:
  - Individual stage execution timings and 10,000-iteration cluster bootstrap 95% confidence intervals over message frames.
  - Confirmed deterministic execution: TX Ed25519 sign = 332.18 ms, RX Ascon decrypt = 0.33 ms, RX Ed25519 verify = 662.67 ms.
- **Hardware Setup**: Standard dual ESP32 testbed (`COM3`, `COM4`), GPIO14 sync pulse wired, sniffer on `COM6`, CPU locked at 160 MHz.

#### Campaign X2: Symmetric AEAD vs Asymmetric Signature Latency
- **Objective / Procedure**: Profiles execution latency scaling of symmetric Ascon-128a AEAD versus asymmetric Ed25519 across payload lengths ranging from 10 B to 58 B.
- **What Was Measured**:
  - Ascon-128a encryption latency: Median 41 µs (TX).
  - Ascon-128a decrypt + verify latency: Median 327 µs (RX).
  - Ed25519 fixed overhead: 332 ms (TX) and 663 ms (RX) independent of chunk size.
- **Hardware Setup**: Dual ESP32 DevKit v1 boards on `COM3` and `COM4` at 160 MHz.

#### Campaign X4: Header Tampering & MAC Fault Injection
- **Objective / Procedure**: Systematically mutates single and multiple bits in unencrypted ESL headers (magic byte, protocol version, message flags, sequence numbers) and authenticated ciphertext tags.
- **What Was Measured**:
  - Tampered header detection rate: 100% dropped at ESL parse stage.
  - Tampered ciphertext detection rate: 100% rejected by Ascon-128a tag verification.
  - Delivery to application buffer: 0 frames.
- **Hardware Setup**: Device A (`COM3`) injecting corrupted frames, Device B (`COM4`) receiver recording error telemetry, `COM6` sniffer verifying over-the-air packets.

#### Campaign X5: Denial-of-Service & High-Rate Burst Flooding
- **Objective / Procedure**: Injects burst floods of unauthenticated and malformed ESP-NOW packets at rates from 10 to 500 frames/sec to test FreeRTOS queue starvation and UI task responsiveness.
- **What Was Measured**:
  - Core 0 UI frame rate: OLED rendering maintained 100% responsiveness without jitter.
  - Core 1 queue overflow: Bounded packet queue drops excess unauthenticated frames cleanly without memory leaks.
  - Region allocator integrity: Zero memory fragmentation or exhaustion during sustained flood.
- **Hardware Setup**: Dual ESP32 boards on `COM3` and `COM4`, traffic injector node flooding radio interface, oscilloscope monitoring UI refresh GPIO.

#### Campaign X6: Forward Error Correction (Hamming(15,11)) Ablation
- **Objective / Procedure**: Measures frame delivery rates and wire efficiency with Hamming(15,11) FEC enabled versus compiled out (`build_fec2` vs `build_bench3`) across clean and attenuator-impaired RF environments.
- **What Was Measured**:
  - Wire overhead: Full profile = 249 B (with FEC) vs 187 B (without FEC); Lite profile = 138 B (with FEC) vs 105 B (without FEC).
  - Single-bit error correction: 100% corrected in hardware without retransmission.
  - Double-bit error detection: 100% detected, triggering clean non-crashing frame drop and ARQ retry.
  - Wire efficiency: 23.3% (Full+FEC) vs 31.0% (Full no-FEC); 42.0% (Lite+FEC) vs 55.2% (Lite no-FEC).
- **Hardware Setup**: Two ESP32 boards on `COM3` and `COM4` flashed alternately with `build_fec2` (FEC on) and `build_bench3` (FEC off).

#### Campaign X7: Frequency Hopping & Channel Convergence
- **Objective / Procedure**: Evaluates channel switching dwell times and packet loss during synchronized pseudo-random hopping across 2.4 GHz channels 1, 6, and 11.
- **What Was Measured**:
  - Channel switch dwell latency: < 15 ms.
  - Rendezvous recovery time: Peer nodes re-converge within one hopping period if desynchronized.
  - Frame drop during hopping: < 2% loss across 500 channel hops.
- **Hardware Setup**: Dual ESP32 boards on `COM3` and `COM4` hopping channels synchronously; `COM6` sniffer tracking channel transitions.

#### Campaign X8: Delivery Accounting & Drop Breakdown
- **Objective / Procedure**: Tracks every transmitted frame over 100 trials under variable RF attenuation to explicitly classify frame outcomes into: sender buffer drops, physical layer drops (bad FCS / RF path loss), and cryptographic tag rejection.
- **What Was Measured**:
  - Packet Delivery Ratio (PDR) vs RF signal degradation.
  - Stop-and-Wait ARQ retry distribution across 5 configured attempts.
  - Decoupled physical loss from authentication refusal: verifies that tag failures are not misclassified as PHY drops.
- **Hardware Setup**: Device A (`COM3`) and Device B (`COM4`) operating through a calibrated step attenuator, with `COM6` sniffer logging ground-truth radio traffic.

#### Campaign X9: Zero-Heap Region Allocator Watermark & Stack Safety
- **Objective / Procedure**: Audits memory safety, static region bump allocator watermarks, and FreeRTOS task stack high-water marks under peak load.
- **What Was Measured**:
  - Application heap allocations (`malloc`/`free`): Exactly **0 bytes**.
  - Static bump region pool watermark: Peak utilization **38.4 KB** of 48.0 KB capacity.
  - Core 0 UI task stack remaining: Minimum 3.2 KB headroom out of 10 KB allocated stack.
  - Core 1 Session task stack remaining: Minimum 4.8 KB headroom out of 16 KB allocated stack.
- **Hardware Setup**: Dual ESP32 DevKit v1 boards running instrumented builds sampling `uxTaskGetStackHighWaterMark()` and `ceepew_region_get_watermark()`.

#### Campaign X10: Over-the-Air Clean Promiscuous Wireshark Capture
- **Objective / Procedure**: Captures pristine 802.11 ESP-NOW action frames using a dedicated sniffer board to generate verifiable PCAPs for Wireshark protocol dissection.
- **What Was Measured**:
  - ESL encapsulation headers: Magic bytes (`0x45 0x53 0x4C 0x01`), sequence numbers, parity nonces.
  - Wire frame ceiling: 249 B MPDU strictly obeying the 250 B ESP-NOW hardware limit.
  - Payload entropy: Validated byte-level Shannon entropy ($7.99\text{ bits/B}$) with complete cleartext absence.
- **Hardware Setup**: Device A (`COM3`) and Device B (`COM4`) paired at 20 cm, third ESP32 sniffer (`COM6`, MAC `58:2a:bd:d8:80:38`) in promiscuous mode running `ceepew_sniffer.bin`.

#### Campaign X11: Multi-Distance Range & RSSI Sweep
- **Objective / Procedure**: Measures signal strength attenuation, packet error rate, and ARQ retransmissions across physical distances from 10 cm to 20 m line-of-sight.
- **What Was Measured**:
  - RSSI attenuation: -28 dBm (at 10 cm) to -88 dBm (at 20 m).
  - Packet Delivery Ratio: 100% PDR from 10 cm to 5 m; ARQ retries activate at $\ge 10\text{ m}$; reliable communication sustained up to 20 m.
- **Hardware Setup**: Stationary Device A (`COM3`) on bench; mobile Device B (`COM4`) placed at calibrated intervals (10 cm, 50 cm, 1 m, 2 m, 5 m, 10 m, 20 m).

#### Campaign X12: Large-Scale Bootstrap Statistical Confidence Intervals
- **Objective / Procedure**: Executes 100+ trials per primitive to generate mathematically sound 95% bootstrap confidence intervals for median execution timings.
- **What Was Measured**:
  - $N \ge 100$ trials per operation; 10,000 cluster-bootstrap resamples.
  - Ed25519 sign 95% CI: $[332.22\text{ ms}, 332.35\text{ ms}]$.
  - Ed25519 verify 95% CI: $[663.20\text{ ms}, 663.33\text{ ms}]$.
  - X25519 ECDH 95% CI: $[147.92\text{ ms}, 148.00\text{ ms}]$.
- **Hardware Setup**: Device A (`COM3`) and Device B (`COM4`) running automated campaign harness at locked 160 MHz.

---

## 3. Evidence Sealing & Provenance Protocol

To ensure reproducible research and prevent post-hoc data tampering:
1. Every experimental run in `data/raw/<run_id>/` contains a `manifest.json` generated by [`tools/bench_manifest.py`](../tools/bench_manifest.py).
2. The manifest records:
   - Run ID, UTC timestamp, git commit hash, and dirty tree status
   - Exact hardware roles (`COM3=Device A`, `COM4=Device B`, `COM6=Sniffer`)
   - Channel configuration, antenna distance, and environmental conditions
   - Compiled firmware flags and application binary SHA-256 hash
   - Pre-declared exclusion rules (written **before** the run commenced)
3. Once a run is sealed, `SHA256SUMS.txt` is generated, and the manifest state transitions permanently from `open` to `sealed`.

To verify the cryptographic integrity of any sealed run:
```bash
python tools/bench_manifest.py verify --run-id BENCHPRIM_20261019
```

---

## 4. Derived Timing Benchmarks

Per-stage execution times on the ESP32 (measured via `esp_timer_get_time()` inside firmware instrumented builds) are recorded in [`data/derived/bench58_today_stages.csv`](derived/bench58_today_stages.csv):

| Stage | Operation Description | Sample Count ($N$) | Median ($\mu\text{s}$) | Median ($\text{ms}$) | Verdict |
|---|---|:---:|:---:|:---:|:---:|
| `sign_us` | Ed25519 Signature Generation (TX) | 114 | 332,183.0 | 332.18 ms | Complete (0 errors) |
| `box_us` | XSalsa20/HMAC Box Opening (RX) | 106 | 1,458.5 | 1.46 ms | 10/10 forged rejected |
| `ascon_us` | Ascon-128a AEAD Decrypt + Verify (RX) | 98 | 327.5 | 0.33 ms | Complete (0 errors) |
| `edverify_us` | Ed25519 Signature Verification (RX) | 99 | 662,674.0 | 662.67 ms | Complete (0 errors) |

---

## 5. Packet Inspection with Wireshark

To inspect captured 802.11 ESP-NOW action frames directly:
1. Open any `.pcap` from `data/raw/` (e.g. `data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap`) in Wireshark.
2. Filter for ESP-NOW action frames:
   ```wireshark
   wlan.fc.type_subtype == 0x000d && wlan_mgt.fixed.category_code == 0x7f
   ```
3. Load the CEE-PEW Wireshark dissector plugin:
   ```bash
   wireshark -X lua_script:tools/capture/wireshark/ceepew_espnow.lua <path_to_pcap>
   ```

---

## 6. Canonical Results Registry

All quantitative claims made in the documentation and paper are mapped directly to entries in the central ledger:
👉 [`results/registry.json`](../results/registry.json)

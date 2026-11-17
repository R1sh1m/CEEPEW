# Over-the-Air (OTA) Adversarial & Fault Injection Testing Guide

This guide documents the physical hardware test setup, attack vectors, and validation methodology used to evaluate the CEE-PEW firmware against over-the-air packet replay attacks, ARQ transmission anomalies, and channel bit errors.

---

## 1. Hardware Architecture & Testbed Topology

The testbed comprises three physical ESP32 nodes connected via USB serial to the test host:

```
  +-----------------------+              +-----------------------+
  |  Device A (DUT Node)  | <==ESP-NOW==> |  Device B (DUT Node)  |
  |  COM4 / Core 0 + 1    |  (Encrypted) |  COM3 / Core 0 + 1    |
  +-----------------------+              +-----------------------+
              ^                                      ^
              |           Promiscuous Sniff          |
              +-------------------+------------------+
                                  |
                                  v
                      +-----------------------+
                      | Sniffer & Injector    |
                      | COM6 / ESP-NOW STA    |
                      +-----------------------+
```

| Node Role | Port | Configuration | Purpose |
|---|---|---|---|
| **DUT Device A** | `COM4` | `ceepew.bin` (Dev Mode) | Initiates session pairing, transmits encrypted messages |
| **DUT Device B** | `COM3` | `ceepew.bin` (Dev Mode) | Pairs with Device A, receives and validates frames |
| **Sniffer / Injector** | `COM6` | `sniffer_minimal.bin` | Promiscuous 802.11 monitor, captures & injects frames OTA |

---

## 2. Test Campaign Modules

### Test A1: Over-the-Air Replay Attack
- **Objective**: Verify that replaying an authentic captured ESP-NOW ciphertext frame over the air is reliably rejected by the receiver's anti-replay gate.
- **Threat Model**: Adversary eavesdrops on air traffic, captures an encrypted command or message, and retransmits it 5 times consecutively.
- **Pass Criterion**: The first transmission is accepted; all 5 replayed transmissions are rejected with error code `CEEPEW_ERR_REPLAY` (`err=6`).
- **Execution**:
  ```powershell
  python tools/extract_frame.py logs/pairing_20261117_064506.txt --out frame.hex
  python tools/sniffer_inject.py --port COM6 --frame frame.hex --repeat 5 --delay 0.05
  ```

### Test A2: Replay Window Boundary Edge Cases
- **Objective**: Exercise the 64-bit sliding window (`replay_window.c`) under non-trivial sequence number distributions.
- **Sub-Tests**:
  - `A2a`: Immediate duplicate of last accepted frame ($\text{seq} = N$) $\rightarrow$ Rejected (`err=6`)
  - `A2b`: Duplicate of older frame within window ($\text{seq} = N - k$, where $k < 64$) $\rightarrow$ Rejected (`err=6`)
  - `A2c`: Frame older than replay window ($\text{seq} < N - 64$) $\rightarrow$ Rejected (`err=6`)
  - `A2d`: Previously unseen sequence number within valid window $\rightarrow$ Accepted
  - `A2e`: Valid frame arriving out-of-order within window $\rightarrow$ Accepted
  - `A2f`: Duplicate arriving after newer sequence numbers accepted $\rightarrow$ Rejected (`err=6`)
  - `A2g`: Replay across session re-key boundary $\rightarrow$ Rejected (`err=6` or auth fail)
  - `A2h`: Replay using previous session's cryptographic key $\rightarrow$ Rejected
- **Execution**:
  ```powershell
  python tools/campaign/run_replay_edge.py --port-a COM4 --port-b COM3 --sniffer-port COM6
  ```

### Test A3: ARQ Duplication vs. Replay Gate Interaction
- **Objective**: Determine layer precedence and verify that transport-layer ARQ retransmissions and anti-replay filtering do not result in double delivery to the user application.
- **Sub-Tests**:
  - `A3a`: Replay packet with duplicate ARQ sequence number $\rightarrow$ Filtered before application dispatch
  - `A3b`: Replay packet with advanced ARQ sequence number $\rightarrow$ Cryptographic / anti-replay rejection
  - `A3c`: Replay packet with corrupted CRC/parity $\rightarrow$ FEC/CRC rejection (`err=17`)
  - `A3d`: Replay during session teardown $\rightarrow$ Dropped cleanly without state corruption
- **Execution**:
  ```powershell
  python tools/campaign/run_arq_duplicate.py --port-a COM4 --port-b COM3 --sniffer-port COM6
  ```

### Test D: Forward Error Correction (FEC) & Channel Characterization
- **Objective**: Measure Hamming(15,11) error-correction performance and Ascon-128 authentication tag rejection under injected channel noise.
- **Scenarios**:
  - `D_clean`: Baseline transmission (0 bit flips) $\rightarrow$ 100% throughput, 0 errors
  - `D_1bit`: Single-bit errors injected $\rightarrow$ 100% corrected by Hamming(15,11)
  - `D_2bit`: Double-bit errors injected $\rightarrow$ 100% detected and dropped by FEC parity
  - `D_3bit`: Multi-bit errors injected $\rightarrow$ 100% detected/rejected (FEC + Ascon auth tag)
  - `D_burst`: Clustered bit errors injected $\rightarrow$ 100% rejected, zero payload corruption
- **Execution**:
  ```powershell
  python tools/campaign/run_fec_analysis.py --log-dir data/raw/D_fec_corruption --out-json data/derived/D_fec_results.json
  ```

---

## 3. Results Ledger Cross-Reference

All quantitative outputs from physical hardware runs are archived in `data/derived/` and registered in `results/registry.json`:
- `data/derived/A1_replay.json`
- `data/derived/A2_edge.json`
- `data/derived/A3_arq.json`
- `data/derived/D_fec_results.json`

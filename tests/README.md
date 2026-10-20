# tests/ — CEE-PEW Multi-Tier Testing & Validation Framework

This directory houses the comprehensive test suites, diagnostic harnesses, and host verification drivers for CEE-PEW. Testing follows a strict multi-tier hierarchy spanning host-side sanitizers, on-device boot self-tests, dual-device protocol integration, and over-the-air capture validation.

---

## Testing Hierarchy Overview

| Tier | Environment | Scope | Harness / Location | Execution Command |
|------|-------------|-------|--------------------|-------------------|
| **Tier 1: Host Unit & Crypto** | PC / Host Native (ASan/UBSan) | Ascon AEAD KAT, Ed25519, HKDF, Huffman, Hamming FEC, Replay Window, Side-channel | `tests/host/` | `python tools/ceepew_pipeline.py --mode host` |
| **Tier 2: Boot Diagnostics** | Target MCU (Core 0 & 1) | 14 hardware/subsystem self-checks executed at boot | `tests/main/`, `tests/components/` | `python tools/ceepew_pipeline.py --mode diag` |
| **Tier 3: Pairing Integration** | Two ESP32 DevKits | BLE discovery, GATT session-code key derivation, handoff to ESP-NOW | `main/session_fsm.c`, `tools/ceepew_monitor.py` | `python tools/ceepew_pipeline.py --mode pairing` |
| **Tier 4: Fuzzing Targets** | Host Native (libFuzzer/AFL) | Protocol frame parsing, Ascon decrypt, Hamming syndrome decoding, ARQ state | `fuzz/`, `tests/host/fuzz_driver.c` | `bash tools/run_fuzz_harnesses.sh` |
| **Tier 5: Production Audit** | Deployment Firmware | eFuse HMAC key, Secure Boot v2, Flash Encryption, diagnostic disablement | `tools/ceepew_production_check.py` | `python tools/ceepew_pipeline.py --mode prod` |
| **Tier 6: OTA Experimental Rig** | Dual ESP32 + Promiscuous Sniffer | Airtime, entropy analysis, cleartext leakage detection, forged frame rejection | `tools/e1_run_mode.py`, `tools/capture/` | `python tools/capture/mode_report.py` |

---

## Tier 1: Native Host Unit & Sanitizer Tests

Location: `tests/host/` (CMake-driven native build with `-fsanitize=address,undefined` on GCC/Clang).

Executes natively without target hardware:
```powershell
python tools/ceepew_pipeline.py --mode host
```

### Coverage by Suite

| Test Target | Primary Primitives Tested | Verification & Pass Criteria |
|-------------|---------------------------|------------------------------|
| `test_security_utils` | Constant-time comparison (`crypto_ct_equal`), secure memory zeroing (`ceepew_secure_zero`) | Memory safety, zero leakage, constant execution flow |
| `test_side_channel` | Timing uniformity of secret-dependent operations | Differential timing rejection, constant execution paths |
| `test_ascon` | Ascon-128 AEAD encryption, decryption, authenticated tag verification | Known-Answer Test (KAT Count 265), tag forgery rejection |
| `test_hamming` | Hamming(15,11) FEC encoding, syndrome decoding | Single-bit correction, double-bit error detection, zero-crash bounds |
| `test_sha256` | SHA-256 compression and streaming hashing | NIST CAVP / RFC known-answer vectors |
| `test_hkdf` | HKDF-SHA256 (RFC 5869) extract and expand | Salt preprocessing, domain separation labels, PRK generation |
| `test_eddsa` | Ed25519 key generation, digital signing, signature verification | RFC 8032 vectors, corrupted signature rejection |
| `test_huffman` | Static Huffman tree encoding and stream decoding | Lossless compression round-trip, out-of-alphabet handling |
| `test_session_pairing` | Three-phase pairing finite state machine transitions | Phase 1 commitment, Phase 2 key derivation, Phase 3 handoff |
| `test_arq_sync` | Stop-and-Wait ARQ sequence tracking and ACK validation | Window sliding, duplicate packet detection |
| `test_arq_fault_injection` | Stop-and-Wait ARQ under synthetic packet drops and timeouts | Retry backoff (100/200/400 ms), effective 5-attempt ceiling |
| `test_replay_window` | 64-bit WireGuard-style sliding window bitmap | In-order acceptance, duplicate rejection, historical frame rejection |
| `test_ui_navigator` | Rotary encoder navigation step computation and rollover | Coordinate wrapping, velocity scaling |

---

## Tier 2: On-Device Boot Diagnostics

Gated by `CONFIG_CEEPEW_DEVELOPMENT_MODE=y` in menuconfig. The firmware compiles in diagnostic self-tests and automatically executes them at boot, emitting a structured, machine-parsable block:

```powershell
python tools/ceepew_pipeline.py --mode diag
```

### The 14 Subsystem Diagnostic Checks

```
=== DIAGNOSTIC REPORT ===
  [Session FSM      ] PASS  # Phase transitions, key exchange, handoff
  [Nonce Enforcement] PASS  # Counter increments, 2^56 hard ceiling enforcement
  [Replay Window    ] PASS  # In-order accept, duplicate reject, old frame drop
  [Pipeline/Memory  ] PASS  # 48 KB static region allocator init, ownership, reset
  [HKDF/Key Derive  ] PASS  # Salt preprocessor and RFC 5869 vector match
  [Transport Hop    ] PASS  # Deterministic PRG 9-channel hopping sync
  [Power/Wakeup     ] PASS  # Light/deep sleep transitions and wake decode
  [Pairing UI       ] PASS  # 4-digit verification code entry and timeout
  [Curve25519 (RFC) ] PASS  # RFC 7748 X25519 test vector on hardware
  [UI Manager       ] PASS  # SSD1306 OLED rendering and I2C recovery
  [UI Cryptogram    ] PASS  # Cryptogram rendering and verification display
  [Pairing Handoff  ] PASS  # BLE-to-ESP-NOW transport transition
  [Pairing Converge ] PASS  # Two-device convergence within 3 attempts
  [Temp Sensor      ] PASS  # On-die silicon temperature sanity check
  Total: 14 / 14 subsystems passed
=========================
```

---

## Tier 3: Two-Device Pairing & Protocol Integration

Verifies the physical interaction between two ESP32 devices over BLE and ESP-NOW:

```powershell
python tools/ceepew_pipeline.py --mode pairing
```

Workflow:
1. Flashes Device B and Device A sequentially.
2. Launches [`tools/ceepew_monitor.py`](../tools/ceepew_monitor.py) across both serial ports simultaneously.
3. Observes BLE advertising, commitment exchange, GATT connection, verification PIN matching, and successful handoff to active ESP-NOW communication.
4. Auto-ingests logs into [`DEVICE_LOG_FINDINGS.md`](../DEVICE_LOG_FINDINGS.md) via [`tools/ceepew_log_pipeline.py`](../tools/ceepew_log_pipeline.py).

---

## Tier 4: Fuzzing Harnesses

Location: `fuzz/` and `tests/host/fuzz_driver.c`.

Off-host fuzzing targets input decoders and parsers using libFuzzer and AFL:
- `fuzz_ascon`: Corrupted ciphertext and mutated associated data (AAD) injection.
- `fuzz_sha256`: Stream boundary fuzzing.
- `fuzz_hamming`: Arbitrary bit-flip injection across 15-bit codewords.
- `fuzz_hkdf`: Variable-length PRK and info field expansion.
- `fuzz_arq`: Sequence counter mutation and ACK desynchronization.

Run all fuzz targets:
```bash
bash tools/run_fuzz_harnesses.sh
```

---

## Tier 5: Production Readiness Audit

Audits the firmware build configuration before production release:

```powershell
python tools/ceepew_pipeline.py --mode prod
```

Validates:
- `CONFIG_CEEPEW_DEVELOPMENT_MODE=n` (diagnostic test suite stripped from binary)
- Flash encryption enabled
- Secure Boot v2 enabled
- Binary memory budget within 4 MB flash partitions

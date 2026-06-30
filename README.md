
<p align="center">
  <img src=".github/banner.svg" alt="CEE-PEW" width="500">
</p>

# CEE-PEW

**C**ryptographic **E**nd-to-**E**nd **P**eer-to-peer **E**ncrypted **W**ireless Communicator

ESP32-based secure messaging with an 8-layer cryptographic stack, pairing via BLE, and encrypted chat over ESP-NOW.

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.1-orange)](https://idf.espressif.com/)
[![Architecture](https://img.shields.io/badge/xtensa-ESP32-blueviolet)](https://www.espressif.com/)

---

## Features

- **End-to-end encrypted messaging** — 8-layer crypto stack (Curve25519 ECDH, Ed25519 EdDSA, HKDF-SHA256, HMAC-SHA256, Ascon-128 AEAD, HMAC-eFuse binding, nonce counter + expiry)
- **User-verified PIN pairing** — 4-character one-time PIN provides ~20 bits of brute-force resistance against active MITM during the ~30 s pairing window
- **Hardware-gated identity handoff** — BLE discovery/pairing, then handoff to ESP-NOW for session transport
- **Forward error correction** — Hamming(15,11) on every frame; selective-repeat ARQ with exponential backoff
- **Zero heap allocation** — 48 KB static region allocator; all session state in compile-time structures
- **Dual-core FreeRTOS** — UI on Core 0, session crypto on Core 1; IPC via FreeRTOS queues only
- **Static Huffman compression** — Lossless message compression
- **ESP-NOW channel hopping** — Frequency diversity for the session transport

## Hardware Requirements

| Component | Specification |
|-----------|--------------|
| MCU | ESP32 DevKit v1 (CP210x UART) |
| Display | SSD1306 128×64 OLED (I2C, SDA=GPIO26, SCL=GPIO27) |
| Input | Rotary potentiometer GPIO33, click button GPIO19 |
| Diagnostic | Push-lock switch GPIO5 (active LOW) |
| Indicator | RGB LED GPIO15/R, GPIO18/G, GPIO23/B (220 Ω to 3.3 V) |

## Getting Started

### Prerequisites

- [ESP-IDF v6.0.1](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32/get-started/)
- Python 3.12+
- ESP32 DevKit v1 with CP210x UART

### Windows

```powershell
# Set up ESP-IDF environment (adjust path for your install)
& "C:\esp\v6.0.1\esp-idf\export.ps1"
idf.py set-target esp32
idf.py build
idf.py -p COM5 flash
idf.py -p COM5 monitor
```

### Linux

```bash
# Set up ESP-IDF environment
source ~/esp/v6.0.1/esp-idf/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor
```

### Two-Device Pairing Test

Two ESP32s are required. Flash both, then run the monitor script:

```bash
pip install pyserial  # if needed
python tools/monitor_both.py --ports COM5,COM6  # Windows
python tools/monitor_both.py --ports /dev/ttyUSB0,/dev/ttyUSB1  # Linux
```

## Project Structure

```
main/                    App entry, session FSM, FreeRTOS tasks
├── main.c              Initialization and task launch
├── session_fsm.c/.h    Three-phase pairing FSM + key derivation
├── task_session.c      Core 1: session crypto and transport
├── task_ui.c           Core 0: OLED rendering and input
├── session_send.c      Message encryption and send pipeline
├── session_msgstore.c  Circular message buffer with TTL expiry
├── session_memory.h    All session state + UI event types
├── ceepew_config.h     Compile-time constants and limits
└── ceepew_security_utils.c  Constant-time comparison, secure zero

components/
├── hal/                Public HAL headers (pins, GPIO, RNG)
├── ceepew_hal/         HAL implementations + UI manager + OLED driver
├── crypto/             Ascon-128 AEAD, HKDF, Ed25519, Curve25519, SHA-256
├── ecc/                Hamming(15,11) FEC, CRC-32, Stop-and-Wait ARQ
├── compress/           Static Huffman coding
├── transport/          BLE transport, ESP-NOW transport, security layer
├── tools/              digital_sum (HKDF salt preprocessor)
└── mem/                Region allocator (48 KB, no heap)

tests/                  On-device test suite (development mode only)
```

## Cryptographic Stack

| Layer | Algorithm | Purpose |
|-------|-----------|---------|
| 1 | X25519 ECDH | Ephemeral shared secret |
| 2 | Ed25519 EdDSA | Peer identity authentication |
| 3 | HKDF-SHA256 | Key derivation (RFC 5869) |
| 4 | HMAC-SHA256 | Commitment binding |
| 5 | Ascon-128 AEAD | Message encryption + authentication |
| 6 | digital_sum | Session-key mixing (HKDF salt preprocessor) |
| 7 | HMAC-eFuse | Device binding via eFuse MAC |
| 8 | Nonce + expiry | Replay prevention (64-bit, 2^56 hard limit) |

## Pairing Protocol

The three-phase pairing protocol establishes trust without persistent infrastructure:

1. **Discovery (BLE)** — Both devices advertise and scan. Beacon commitment exchange (truncated SHA-256 over session_code + device IDs). Lower-MAC peer becomes initiator.

2. **Pairing (BLE GATT)** — Each device encrypts its Ed25519 sign_pk + Curve25519 box_pubkey + WiFi MAC using Ascon-128 AEAD. Keys derived from the user-entered session code via HKDF. Both devices display a 4-character PIN; the user verifies the PIN matches on both. This provides ~20 bits of brute-force resistance against active MITM during the ~30 s pairing window.

3. **Handoff (ESP-NOW)** — BLE torn down, ESP-NOW initialized, peer registered using WiFi MAC exchanged in Phase 2. Session FSM enters ACTIVE state.

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full system overview, including the memory model, task architecture, and component map.

## Security

See [SECURITY.md](SECURITY.md) for the threat model, known limitations, and responsible disclosure policy.

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

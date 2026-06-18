# CEE-PEW

Cryptographic End-to-End Peer-to-peer Encrypted Wireless Communicator. An ESP32-based secure messaging device that implements an 8-layer cryptographic stack over BLE (pairing) and ESP-NOW (session transport) with custom Hamming(15,11) FEC, Stop-and-Wait ARQ, region-based memory allocation, and a dual-core FreeRTOS architecture. Designed for peer-to-peer encrypted communication between two devices using a 4-character PIN confirmation for MITM resistance during pairing.

**What makes it interesting:** 8-layer crypto stack (Curve25519 ECDH, Ed25519 EdDSA, HKDF-SHA256, HMAC-SHA256, Ascon-128 AEAD, proprietary digital_sum_mix, HMAC-eFuse binding, nonce counter + expiry), custom pairing protocol with beacon commitment exchange, ESP-NOW for data transport with channel hopping, Hamming(15,11) forward error correction on every frame, region allocator (no heap), and a dual-core task architecture communicating only via FreeRTOS queues.

**Target hardware:** ESP32 DevKit v1 (CP210x UART) with SSD1306 128×64 OLED (I2C, SDA=GPIO26, SCL=GPIO27), rotary potentiometer (GPIO33), click button (GPIO19), DIAG switch (GPIO5), RGB LED (GPIO15/18/23).

## Hardware Requirements
- ESP32 DevKit v1 (CP210x UART)
- SSD1306 128×64 OLED (I2C, SDA=GPIO26, SCL=GPIO27)
- Rotary potentiometer on GPIO33 (ADC1_CH5)
- Click button on GPIO19 (INPUT_PULLUP, active LOW)
- DIAG push-lock switch on GPIO5 (INPUT_PULLUP, active LOW)
- Common-cathode RGB LED on GPIO15 (Red), GPIO18 (Green), GPIO23 (Blue) with 220Ω resistors to 3.3V

## Getting Started
### Prerequisites
- ESP-IDF v6.0.1 (installed via `eim` at `C:\Espressif`)
- Python 3.12
- Windows 11 (PowerShell) or Linux

### Build & Flash
```powershell
. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1
idf.py build
idf.py -p COM5 flash monitor
```

## Project Structure
```
main/                    App entry, FSM, FreeRTOS tasks, on-device tests
components/
  hal/                   PUBLIC headers only (hal_pins.h, hal_gpio.h, hal_rng.h)
  ceepew_hal/            HAL implementations + UI manager + display layer
  crypto/                TweetNaCl/Curve25519, Ascon-128, HKDF, Ed25519, RNG
  ecc/                   Hamming(15,11) FEC, CRC-32, Stop-and-Wait ARQ
  compress/              Static Huffman coding
  transport/             BLE (discovery/pairing), ESP-NOW (chat), channel hop
  tools/                 digital_sum (HKDF salt preprocessor)
  mem/                   Region allocator, typed pipeline
```

## Architecture
See [ARCHITECTURE.md](ARCHITECTURE.md) for the full system overview, three-phase pairing protocol, cryptographic stack, transport stack, memory model, task architecture, and component map.

## Security
See [SECURITY.md](SECURITY.md) for threat model, known limitations, responsible disclosure, and build security notes.


//fix the license type, create proper ascii art banner, and add build instructions for Linux in the README. Also add a note about the 4-character PIN providing ~20 bits of brute-force resistance against an active MITM during the pairing window. also need to address the various notes, confusions and design choices etc. doesn't really give detailed insight into the design and implementation, just a high-level overview. need to add more details about the crypto stack, transport stack, memory model, task architecture, and component map. also need to add a section on security considerations, threat model, and known limitations.
no detailed installs and build explanation, lack of flairs etc for GitHub

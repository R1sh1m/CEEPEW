# CEE-PEW Architecture

## System Overview
Two devices, symmetric roles. Device A initiates, Device B responds.
Phases: Discovery → Pairing → Session.

## Three-Phase Pairing Protocol

**Phase 1: Discovery** — BLE advertisement (manufacturer-specific AD records in scan response).
Both devices advertise and scan simultaneously. A scans, finds B, extracts public key material from the beacon commitment (truncated SHA-256 of session code + device IDs). The lower-MAC peer becomes the initiator.

**Phase 2: Pairing** — Commitment exchange and user-confirmed PIN.
Once both commitments match, the devices enter a GATT-based identity exchange. Each device encrypts its Ed25519 sign_pk + Curve25519 box_pubkey + WiFi MAC using Ascon-128 AEAD with a key derived from the session code + sorted device IDs. The initiator opens a brief GATTC connection to write to the peer's 0xFFF3 characteristic. After successful delivery, both devices display a 4-character PIN. Users confirm the PIN matches on both devices by pressing the button. This provides ~20 bits of brute-force resistance against active MITM during the pairing window.

**Phase 3: Handoff** — BLE torn down. ESP-NOW peer registered. Session FSM advances.
Both devices send a "HANDOFF_READY" beacon over BLE and wait for the peer's beacon (synchronization). Once both are received, BLE is fully torn down (controller + bluedroid deinit), WiFi is initialized in STA mode, ESP-NOW is started on a fixed channel (default: 1), and the peer is registered using the **WiFi MAC** (not BLE MAC) exchanged during Phase 2. Session FSM enters ACTIVE state.

## Cryptographic Stack (8 Layers)
1. **ECDH (Curve25519)** — Ephemeral key agreement for session key derivation
2. **EdDSA (Ed25519)** — Peer identity authentication via sign_pk exchange
3. **HKDF (SHA-256)** — Key derivation from ECDH shared secret + session code
4. **HMAC (SHA-256)** — Commitment binding in pairing (beacon = truncate(HMAC))
5. **Ascon-128 AEAD** — Data encryption in session (payload + associated data)
6. **digital_sum_mix** — Proprietary session key mixing (HKDF salt preprocessor)
7. **HMAC-eFuse binding** — Device-bound key material from eFuse MAC
8. **Nonce counter + expiry** — Replay prevention (64-bit counter, 2^56 hard limit)

## Transport Stack
**BLE (pairing only) → ESP-NOW (session)**
- FEC: Hamming (15,11) over every ESP-NOW frame (3 parity bytes per 11 data bytes)
- ARQ: Selective-repeat with exponential backoff (ecc_arq.c, max 3 retries, 500ms base timeout)
- Hop abstraction: `transport_hop.c` unifies BLE and ESP-NOW behind one API (`hop_send`, `hop_recv`, `hop_switch_transport`)

## Memory Model
- **Region allocator** (`ceepew_region.c`): 48KB static pool, no heap allocation ever
- **Pipeline** (`ceepew_pipeline.c`): Typed slots over the region pool for zero-copy message passing
- All session state in `session_memory.h` — single struct, no dynamic allocation

## Task Architecture
- **Core 0**: `task_ui` (OLED rendering + input debounce, 4KB stack)
- **Core 1**: `task_session` (crypto + FSM + session logic, 8KB stack)
- **IPC**: FreeRTOS queues only (`g_session_rx_queue`, `g_ui_event_queue`) — no shared globals except `session_memory.h`

## Component Map
```
┌─────────────────────────────────────────────────────────────────┐
│                        APPLICATION LAYER                        │
│  main.c  →  task_session.c (Core 1)   task_ui.c (Core 0)       │
│           session_fsm.c             ui_manager.c                │
│           session_send.c            hal_ui.c                    │
│           session_msgstore.c        layout.c                    │
│           session_memory.h                                     │
├─────────────────────────────────────────────────────────────────┤
│                      TRANSPORT LAYER                            │
│  transport_hop.c ←→ transport_espnow.c (Phase 3)               │
│                     transport_ble.c (Phases 1-2)                │
│                     transport_ble_gatt_crypto.c (Phase 2)      │
│                     transport_esl.c (ESP-NOW Security Layer)   │
│                     transport_replay.c (WireGuard-style bitmap)│
├─────────────────────────────────────────────────────────────────┤
│                      CRYPTOGRAPHIC LAYER                        │
│  crypto_box_wrap.c (XSalsa20+Poly1305)   crypto_ascon.c (AEAD) │
│  crypto_hkdf.c (RFC5869)                 crypto_eddsa.c        │
│  crypto_sha256.c                         crypto_rng.c        │
│  crypto_ctx.c (key derivation orchestrator)                    │
│  ceepew_security_utils.c (secure zero)                         │
├─────────────────────────────────────────────────────────────────┤
│                      RELIABILITY LAYER                          │
│  ecc_hamming.c (FEC 15,11)           ecc_arq.c (ARQ)           │
│  ecc_crc32.c                                                          │
├─────────────────────────────────────────────────────────────────┤
│                      COMPRESSION LAYER                          │
│  compress_huffman.c (Static Huffman)                           │
├─────────────────────────────────────────────────────────────────┤
│                      HARDWARE ABSTRACTION                       │
│  hal_radio.c (WiFi/ESP-NOW)         hal_adc.c (Potentiometer)  │
│  hal_gpio.c (Button/DIAG)           hal_rgb.c (RGB LED)        │
│  hal_i2c.c (OLED)                   hal_timer.c                │
│  hal_rng.c (TRNG)                   hal_efuse.c (MAC/eFuse)    │
│  ui_manager.c (20+ UI states)       task_arch.c (Core pinning) │
├─────────────────────────────────────────────────────────────────┤
│                      MEMORY MANAGEMENT                          │
│  ceepew_region.c (48KB pool)      ceepew_pipeline.c (slots)    │
└─────────────────────────────────────────────────────────────────┘
```
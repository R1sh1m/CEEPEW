# transport — BLE (Pairing) → ESP-NOW (Session)

## BLE vs ESP-NOW Lifecycle

| Phase | Transport | Purpose |
|-------|-----------|---------|
| 1 Discovery | BLE | Advertise + scan, exchange commitment beacons (truncated HMAC) |
| 2 Pairing | BLE + GATT | Commitment verification, Ed25519 sign_pk exchange (encrypted via `transport_ble_gatt_crypto`), 4-char PIN confirmation |
| 3 Session | ESP-NOW | Ascon-128 AEAD + Hamming(15,11) FEC + ARQ + channel hopping |

**Key point**: BLE is **torn down completely** before ESP-NOW starts. No concurrent BLE + ESP-NOW operation.

## How `transport_hop.c` Abstracts Both

`transport_hop.c` provides a unified API:
- `hop_send(data, len)` — routes to BLE (Phases 1-2) or ESP-NOW (Phase 3)
- `hop_recv(buf, max_len)` — receives from active transport
- `hop_switch_transport(TRANSPORT_ESPNOW)` — called during Phase 3 handoff

The session FSM calls `hop_switch_transport()` only once, after both peers send HANDOFF_READY beacons and BLE is fully deinitialized.

## Files

| File | Purpose |
|------|---------|
| `transport_ble.c/h` | **ACTIVE** BLE transport (Phases 1-2). Advertising, scanning, GATT sign_pk exchange, HANDOFF_READY beacon sync. |
| `transport_ble_gatt_crypto.c/h` | GATT sign_pk encryption wrapper. Ascon-128 AEAD of `sign_pk \|\| box_pubkey \|\| wifi_mac` using key derived from session_code + sorted device IDs. Called by `transport_ble.c`. |
| `transport_espnow.c` | ESP-NOW transport (Phase 3). WiFi init, ESP-NOW init, peer registration (with **WiFi MAC**, not BLE MAC), channel hopping task. |
| `transport_esl.c` | ESP-NOW Security Layer. Ascon-128 AEAD encrypt/decrypt, nonce management, replay window (WireGuard-style 64-bit bitmap). |
| `transport_replay.c` | Replay protection bitmap. 64-bit sliding window, constant-time check. |
| `transport_hop.c` | Transport abstraction layer. Unifies BLE + ESP-NOW behind one API. |

## BLE Dual-Path Note (Resolved)
Previously there were two BLE implementations:
- `transport_ble.c` — original (advertising + scanning + GATT)
- `transport_ble_gatt_crypto.c` — crypto wrapper for GATT

**Resolution**: `transport_ble.c` is the ACTIVE transport. `transport_ble_gatt_crypto.c` is a **crypto helper** called by `transport_ble.c` during GATT sign_pk exchange. They are not alternative paths — they work together.

## ESP-NOW Stability Fixes (Part 3)
See the sprint brief for 5 root causes and fixes:
- A: BLE not fully torn down before ESP-NOW init
- B: WiFi not initialized / wrong mode
- C: Wrong MAC used for peer registration (BLE MAC vs WiFi MAC)
- D: FSM race condition in handoff (added HANDOFF_SYNC state)
- E: ARQ/send loop not waiting for send callback (added semaphore gate)

## Channel Hopping
- 9 channels (`CEEPEW_HOP_CHANNELS`), 5 second interval (`CEEPEW_HOP_INTERVAL_MS`)
- PRG sequence derived from session key + nonce counter
- Both devices must hop in perfect sync (nonce counter drives channel selection)
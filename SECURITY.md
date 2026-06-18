# Security Policy

## Responsible Disclosure
Report security vulnerabilities via GitHub issue with the "security" label, or email the maintainer directly. Do not disclose vulnerabilities publicly until a fix is available.

## Threat Model
CEE-PEW is designed to resist:
- **Passive eavesdropping** — Ascon-128 AEAD provides authenticated encryption for all session traffic
- **Active MITM during pairing** — EdDSA identity binding + 4-character PIN confirmation (user-verified). The PIN provides approximately 20 bits of brute-force resistance against an active MITM during the pairing window
- **Replay attacks** — 64-bit nonce counter with WireGuard-style replay window + session expiry at `CEEPEW_NONCE_HARD_LIMIT` (2^56)
- **Cloning attacks** — HMAC-eFuse binding ties derived keys to the device's unique eFuse MAC

CEE-PEW is NOT designed to resist:
- **Physical hardware attacks** — No secure enclave, no tamper detection, keys in SRAM
- **Side-channel attacks on Xtensa LX6** — No constant-time guarantees for all operations (timing, power analysis)
- **Attacks requiring code execution on the device** — No secure boot / flash encryption enforced in this build
- **Denial of service** — Radio jamming, BLE/ESP-NOW flooding not mitigated

## Known Limitations
1. **4-character PIN confirmation** provides approximately 20 bits of brute-force resistance against an active MITM during the pairing window. An attacker with radio access during the ~30 second pairing window could attempt ~1M guesses.
2. **No forward secrecy for pairing phase** — The session code is the sole entropy source for Phase 2 key derivation. If the session code is compromised, past pairings can be decrypted.
3. **ESP-NOW LMK derived from session key** — If the session key is extracted from one device, all past/future ESP-NOW traffic with that peer is decryptable.
4. **No secure boot / flash encryption** — Production deployments should enable both in `sdkconfig.production`.
5. **BLE advertisement commitment** — The truncated 16-byte commitment in the BLE scan response could theoretically be brute-forced offline (2^128 preimage resistance, but truncated to 128 bits).
6. **Device identity = ephemeral Ed25519 keypair** — No long-term identity; MITM resistance relies entirely on the user-verified PIN.

## Build Security Notes
- `sdkconfig` is gitignored. Use `sdkconfig.debug` for debug builds, `sdkconfig.production` for releases.
- Never commit `keys/` directory — it is gitignored by design.
- `CEEPEW_DEBUG_SERIAL` must be undefined in production builds (enforced by `sdkconfig.production`).
- Region allocator pool (48KB) is static — no heap allocation means no malloc-related vulnerabilities.
- All secret material (session_key, sign_sk, peer_sign_pk) is secure-zeroed on teardown via `ceepew_secure_zero()` (volatile pointer + memory barrier pattern).
- Constant-time comparison (`crypto_ct_equal`) used for all tag/MAC/key comparisons — never `memcmp`.
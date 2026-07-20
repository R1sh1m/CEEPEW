# Security Policy

## Responsible Disclosure
Report security vulnerabilities via GitHub issue with the "security" label, or email the maintainer directly. Do not disclose vulnerabilities publicly until a fix is available.

## Threat Model
CEE-PEW is designed to resist:
- **Passive eavesdropping** — Layered authenticated encryption: Ascon-128 AEAD (inner) + XSalsa20-Poly1305 over X25519 ECDH (outer) for all session traffic
- **Active MITM during pairing** — EdDSA identity binding + 4-character PIN confirmation (user-verified). The PIN provides approximately 20 bits of brute-force resistance against an active MITM during the pairing window
- **Replay attacks** — 64-bit nonce counter with WireGuard-style replay window + session expiry at `CEEPEW_NONCE_HARD_LIMIT` (2^56)
- **Cloning attacks** — HMAC-eFuse binding ties derived keys to the device's unique eFuse MAC. **Requires manufacturing-time provisioning:** a unique 256-bit key must be burned into EFUSE_BLK1. On stock dev kits (unprovisioned), the device secret falls back to SHA-256(MAC || salt), which is deterministic from the public MAC and does not resist cloning.

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
7. **Identity-degraded (no-Ed25519) fallback mode** — If the peer's Ed25519 signature public key (`sign_pk`) is never received over the BLE GATT channel after `CEEPEW_MAX_RECONNECT_ATTEMPTS` (5) retries, the pairing attempt transitions to a dedicated `UI_STATE_PAIRING_DEGRADED` screen rather than silently continuing. The user must explicitly press the button (short click) to confirm degraded operation, or long-press (1.5 s) to cancel entirely. In degraded mode:
    - **Ed25519 signature verification is permanently disabled** for the session. Incoming frames are authenticated only by Ascon-128 AEAD tag verification and the X25519 ECDH-derived box layer — no per-frame identity binding via Ed25519.
    - **All chat UI screens display a persistent "UNVERIFIED IDENTITY" banner** (analogous to Signal/WhatsApp "safety number changed" warnings) indicating that peer identity is not cryptographically verified.
    - The `session_is_identity_degraded()` flag persists for the session duration and is visible to UI, transport, and crypto layers.
    - This mode exists solely as an accessibility/reliability fallback for environments where BLE GATT connections are unreliable (e.g., high RF interference, incompatible BLE controller firmware). It is never the default path and requires active user consent on every occurrence.
8. **mbedTLS dependency for SHA-256** — SHA-256 and HMAC-SHA256 (and therefore HKDF, commitments, and nonce derivation) delegate the underlying hash to mbedTLS via the ESP-IDF PSA API. This is a mature, well-audited implementation bundled with ESP-IDF, but it means the library's security posture includes mbedTLS as a dependency. The following primitives are standalone (no mbedTLS): Ascon-128 AEAD, X25519 ECDH, Ed25519 EdDSA, XSalsa20 stream cipher.
9. **Nested AEAD structure** — Session messages pass through two authenticated encryption layers: Ascon-128 AEAD (inner, keyed from the HKDF session key) then XSalsa20-Poly1305 over X25519 ECDH (outer, keyed from a fresh CSPRNG-derived ephemeral keypair). The two layers use independently-derived encryption keys (different HKDF outputs / different ECDH computations), but share the same nonce base (session_id + counter). This layered approach provides defense-in-depth but is not a standard construction; it may be simplified to a single AEAD layer in a future revision.

## Build Security Notes
- `sdkconfig` is gitignored. Use `sdkconfig.debug` for debug builds, `sdkconfig.production` for releases.
- Never commit `keys/` directory — it is gitignored by design.
- `CONFIG_CEEPEW_DEVELOPMENT_MODE` must be `n` in production builds (enforced by `sdkconfig.production`). This master toggle controls test compilation, `CEEPEW_DEBUG_SERIAL` (operational logging), and `CEEPEW_HEADLESS_MODE` (UI auto-advance).
- No sensitive material (keys, session codes, HKDF intermediates, plaintext content) is ever logged, even when `CEEPEW_DEBUG_SERIAL` is enabled. Device MAC addresses are logged at INFO level for diagnostic purposes — these are not secret (they are transmitted in the clear over BLE and WiFi), but are included here for transparency.
- Region allocator pool (48KB) is static — no heap allocation means no malloc-related vulnerabilities.
- All secret material (session_key, sign_sk, peer_sign_pk) is secure-zeroed on teardown via `ceepew_secure_zero()` (volatile pointer + memory barrier pattern).
- Constant-time comparison (`crypto_ct_equal`) used for all tag/MAC/key comparisons — never `memcmp`.
- mbedTLS provides SHA-256 / HMAC-SHA256 via the PSA API — keep `mbedtls` up to date with ESP-IDF releases for security patches. The PSA crypto init warm-up (`crypto_ctx.c`) is a known requirement on ESP32.
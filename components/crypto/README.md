# crypto — Cryptographic Primitives

This component provides all cryptographic primitives for CEE-PEW. No external crypto libraries (mbedTLS, wolfSSL) are used — all algorithms are implemented directly or ported from reference implementations.

## Files

| File | Purpose |
|------|---------|
| `crypto_ctx.h/c` | Master context orchestrator. `crypto_ctx_init()` derives all session keys from ECDH + session code. `crypto_ctx_derive_keys()` runs the HKDF tree. `crypto_ctx_destroy()` secure-zeros all key material. |
| `curve25519.h/c` | TweetNaCl Curve25519 scalar multiplication (`crypto_scalarmult`). Used for ECDH key agreement. |
| `crypto_ecdh.h/c` | Ephemeral ECDH keypair generation and shared secret computation. |
| `crypto_eddsa.h/c` | Ed25519 signing/verification (port of TweetNaCl). Used for peer identity auth. |
| `crypto_box_wrap.h/c` | XSalsa20+Poly1305 (NaCl `crypto_box`) wrapper for message encryption. |
| `crypto_ascon.h/c` | Ascon-128 AEAD (NIST LWC finalist). Used for ESP-NOW session encryption + GATT sign_pk encryption. |
| `crypto_hkdf.h/c` | HKDF-SHA256 (RFC 5869). Key derivation for all session keys, PMK/LMK, GATT keys. |
| `crypto_hmac.h/c` | HMAC-SHA256. Used for pairing commitment binding. |
| `crypto_hmac_efuse.h/c` | HMAC bound to eFuse MAC. Device-bound key derivation. |
| `crypto_sha256.h/c` | SHA-256 hash. Used for commitments, HKDF, nonce derivation. |
| `crypto_rng.h/c` | TRNG wrapper (`esp_fill_random`) + health check (3-sample repetition test). |
| `crypto_pad.c` | PKCS#7 padding (64-byte block) for `crypto_box` input. |
| `crypto_stream.h/c` | XSalsa20 stream cipher (used internally by `crypto_box_wrap`). |

## Initialization Order
1. `crypto_rng_health_check()` — at boot, before any key generation
2. `crypto_ctx_init()` — after Phase 2 pairing completes (has session_code + peer MACs)
3. `crypto_ctx_derive_keys()` — called internally by `crypto_ctx_init`
4. `crypto_ctx_destroy()` — on session end / device wipe

## External Dependencies
None. Pure C11 + ESP-IDF `esp_fill_random` for entropy. All other operations are constant-time where secret-dependent.

## Constants (from `main/ceepew_config.h`)
- `CEEPEW_NONCE_HARD_LIMIT` = 2^56 (session terminates here)
- `CEEPEW_SESSION_KEY_BYTES` = 16 (Ascon-128 key)
- `CEEPEW_COMMITMENT_BYTES` = 16 (truncated HMAC for beacon)
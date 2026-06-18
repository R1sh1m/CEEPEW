# ecc — Error Correcting Codes (NOT Elliptic Curve Crypto)

**Important:** Despite the name "ecc", this component implements **Error Correcting Codes**, not Elliptic Curve Cryptography. Elliptic curve crypto (Curve25519, Ed25519) lives in `components/crypto/`.

## Files

| File | Purpose |
|------|---------|
| `ecc_hamming.h/c` | Hamming(15,11) Forward Error Correction. Encodes 11 data bits into 15 bits (4 parity). Corrects any single-bit error, detects double-bit errors. Applied to **every ESP-NOW frame** before transmission. |
| `ecc_crc32.h/c` | CRC-32 (Castagnoli polynomial 0x1EDB1EDC6F). Used for frame integrity check before FEC decode. |
| `ecc_arq.c` | Stop-and-Wait ARQ with exponential backoff. Manages retransmissions for unacknowledged ESP-NOW frames. Max 3 retries (`CEEPEW_ARQ_MAX_RETRIES`), base timeout 500ms (`CEEPEW_ARQ_TIMEOUT_MS`). |

## How They Work Together (Outbound Frame Pipeline)
```
plaintext
  → PKCS7 pad to 64B
  → crypto_box (XSalsa20+Poly1305)
  → Ascon-128 AEAD outer
  → Hamming(15,11) FEC (session-permuted parity positions)
  → CRC-32
  → ARQ wrapper (sequence number + retry logic)
  → ESP-NOW (on PRG-hopped channel)
```

## Hamming(15,11) Details
- **Codeword**: 15 bits = 11 data + 4 parity
- **Parity positions**: 1, 2, 4, 8 (powers of 2)
- **Permutation**: Parity bit positions are session-permuted using `digital_sum_mix(session_key)` to prevent targeted bit-flip attacks
- **Correction**: Single-bit error correction via syndrome lookup table
- **Overhead**: 36% (15/11 ≈ 1.36)

## ARQ Details
- **Protocol**: Stop-and-Wait (one frame in flight at a time)
- **Retries**: 3 maximum (configurable via `CEEPEW_ARQ_MAX_RETRIES`)
- **Backoff**: Exponential with jitter: 500ms, 1000ms, 2000ms base + random jitter
- **ACK**: Implicit — next expected sequence number acknowledged by successful decrypt of subsequent frame
- **Timeout**: 500ms base (`CEEPEW_ARQ_TIMEOUT_MS`)

## No malloc
All state is stack-allocated or in session memory. No dynamic allocation.
# CEE-PEW — Full System Architecture Document
### Cryptographic End-to-End Peer-to-peer Encrypted Wireless Communicator
**Version:** 1.0 | **Status:** Pre-development Specification

---

## Table of Contents
1. HX-63 Inspired Feedback System
2. Error Correction Architecture (Full Detail)
3. System-Wide Module Map
4. Module Specifications & Interfaces
5. Data Structures & Memory Layout
6. State Machine Specification
7. Attack Resistance Mapping
8. Coding Standards (JPL-Derived Rules)
9. Build & Static Analysis Pipeline

---

## 1. HX-63 Inspired Feedback System

### 1.1 Philosophy
The HX-63 had 10^600 possible configurations — it made the operator *feel* the weight
of that security through physical complexity: nine rotors, two plugboards, reinjection paths.
CEE-PEW must do the same via its OLED UI. Every cryptographic state transition is
visualized. The user is never left wondering what the device is doing. Transparency is
a security feature: it builds trust and makes anomalies visible.

### 1.2 The Security Complexity Display ("CRYPTOGRAM PANEL")
Displayed after successful key exchange, and accessible at any time by holding the
button for 2 seconds. Inspired directly by HX-63's configuration complexity readout.

```
┌──────────────────────────────┐
│  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ 100%   │  ← Security strength bar
│  ASCON-128  |  CURVE25519    │
│  Ed25519    |  HKDF-SHA256   │
│  KEY: A3F7..B2C1             │  ← Truncated session key fingerprint
│  NONCE: 0x00000042           │  ← Current message nonce counter
│  ENTROPY: 256 bit            │
│  CONFIG SPACE: 2^256         │  ← Equivalent to HX-63's 10^600 display
│  SESSION: 00:07:33 alive     │  ← Session uptime
│  MSG TTL: 08:27 remaining    │  ← Time until auto-wipe
│  SIG: ✓ VERIFIED             │  ← Last message signature status
└──────────────────────────────┘
```

**Config Space Calculation (display only, computed once at session start):**
The 256-bit session key space is 2^256 ≈ 1.16 × 10^77 configurations.
Combined with the Ed25519 keypair space (2^252) and the nonce counter space (2^64),
the effective configuration space displayed is the product — shown as a power of 2
and its decimal approximation. This gives the operator the same visceral sense of
scale that HX-63 operators felt looking at their rotor configurations.

### 1.3 Per-Operation Feedback States

no audio output of any kind. All feedback is delivered via the OLED
display and the RGB LED exclusively.

| Operation | OLED Animation | RGB LED |
|---|---|---|
| Boot | Lock assembles pixel-by-pixel | W→R→G→B→W sweep once |
| Discovery | Radar sweep, dots appear | Slow Blue pulse (1 Hz) |
| Code entry | Scrolling selector, blink cursor | Fast Blue pulse (4 Hz) |
| Key exchange | Progress bar, "DERIVING KEY..." | Slow Yellow blink |
| Channel secure | Lock closes, fingerprint shown | Solid Green (stays on) |
| Message compose | Character grid, live preview | Dim Green (session active) |
| Encrypting | Spinning ▓ bar, "ENCRYPTING..." | Brief White flash (100 ms) |
| Transmitting | Arrow animation, "TX..." | Brief Cyan flash (100 ms) |
| Received | "◉ RX" blink, "DECRYPTING..." | Brief Cyan double-flash |
| Sig verified | "✓ SIG OK" checkmark | Single Green blink (200 ms) |
| Sig FAILED | "✗ SIG FAIL" — inverted screen | 3× rapid Red flash |
| Error corrected | "⚠ ECC FIX" shown briefly | Single Orange blink (200 ms) |
| Uncorrectable error | "✗ PKT DROP" shown | 2× Red flash |
| Auto-wipe | Shredder animation "WIPING..." | Rapid Red blink until done |
| Session end | Lock opens, "CHANNEL CLOSED" | Blue fade-out (300 ms) → OFF |

### 1.4 Real-Time Security Anomaly Indicators
These display in a persistent status bar on the bottom 8 pixels of the OLED:
- `[E]` — Error correction was invoked on last packet
- `[!]` — Nonce approaching rollover (warn at 2^63, hard stop at 2^64 - 1)
- `[R]` — Replay detected (sequence number already seen)
- `[T]` — Session TTL below 10% of configured duration
- `[W]` — Message wipe imminent (< 60 seconds remaining)

---

## 2. Error Correction Architecture (Full Detail)

### 2.1 Design Philosophy
Error correction in CEE-PEW is a **four-layer defense-in-depth stack**. Each layer
serves a distinct purpose. They are not redundant — they operate on different
representations of the data at different stages of the pipeline.

```
PLAINTEXT
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  LAYER 0: Input Validation                          │
│  Bounds check, parameter validation, type safety    │
└─────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  LAYER 1: Huffman Compression                       │
│  Reduces size before encryption                     │
└─────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  LAYER 2: SHA-256 Plaintext Digest                  │
│  Cryptographic integrity anchor before encryption   │
└─────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  LAYER 3: Ascon-128 AEAD                            │
│  Encryption + authentication tag (128-bit MAC)      │
│  Primary integrity + confidentiality guarantee      │
└─────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  LAYER 4: Systematic Linear Code (Parity Matrix)    │
│  (15,11) Extended Hamming — detect 2, correct 1     │
│  Applied to the CIPHERTEXT packet                   │
└─────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  LAYER 5: CRC-32 Frame Check                        │
│  Transport-layer error detection on full frame      │
└─────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  LAYER 6: ARQ Protocol (Stop-and-Wait)              │
│  Sequence numbers, ACK/NACK, bounded retransmit     │
└─────────────────────────────────────────────────────┘
    │
    ▼
CIPHERTEXT PACKET (transmitted via ESP-NOW)
```

### 2.2 Layer 1 — Huffman Compression

**Purpose:** Reduce plaintext size before encryption to minimize transmission time
and power consumption. Also provides mild Shannon entropy normalization.

**Implementation:** Static Huffman table (trained offline on English letter frequency
corpus). No dynamic tree construction at runtime — table stored in flash (ROM).

**Static Huffman Code Table (representative, stored as uint8_t arrays in flash):**
```
Symbol  | Frequency | Code      | Length
--------|-----------|-----------|-------
' '     | 13.00%    | 00        | 2
'e'     | 12.70%    | 010       | 3
't'     | 9.06%     | 011       | 3
'a'     | 8.17%     | 1000      | 4
'o'     | 7.51%     | 1001      | 4
...     | ...       | ...       | ...
rare    | <0.5%     | 1111xxxx  | 8-12
```

**Bound:** Maximum output size is bounded to `CEEPEW_MAX_MSG_BYTES * 1.2` (worst case,
incompressible input expands by at most 20%). This is a static compile-time constant.
Compression is skipped if output would exceed input (stored uncompressed with flag bit).

**Struct:**
```c
typedef struct {
    uint8_t  data[CEEPEW_HUFF_BUF_MAX];  /* static buffer, no heap */
    uint16_t bit_length;                  /* total bits used        */
    bool     compression_applied;         /* false = passthrough    */
} HuffResult_t;
```

### 2.3 Layer 2 — SHA-256 Plaintext Digest

**Purpose:** Bind the plaintext hash into the packet before encryption. On decryption,
recompute and compare. This provides an independent integrity check separate from the
Ascon authentication tag. If Ascon's tag passes but SHA-256 fails, it indicates a
firmware bug or active tampering at the crypto layer — a critical anomaly.

**Implementation:** mbedTLS `mbedtls_sha256()` (already in ESP-IDF, hardware-accelerated
on ESP32 via SHA accelerator peripheral).

**Stored as:** First 32 bytes of the plaintext payload before Ascon encryption.
The 32-byte digest is encrypted alongside the message — it is never transmitted in clear.

### 2.4 Layer 3 — Ascon-128 AEAD (Primary Integrity + Confidentiality)

**Mode:** Ascon-128 (NIST SP 800-232, finalized August 2025).

**Parameters:**
- Key: 128-bit session key (derived via HKDF)
- Nonce: 128-bit, constructed as: `[64-bit session_id || 64-bit message_counter]`
  - `session_id` = first 64 bits of HKDF output
  - `message_counter` = monotonically incrementing uint64_t, starts at 1
  - **Counter NEVER resets within a session.** Session MUST terminate before counter
    reaches `UINT64_MAX - CEEPEW_NONCE_WARN_THRESHOLD`. Hard stop enforced by assertion.
- Associated Data (AD): The packet header (sequence number + timestamp + device_id)
  This binds the header to the ciphertext — modifying the header invalidates the tag.
- Authentication Tag: 128-bit, appended to ciphertext.

**Nonce reuse prevention (attack #13 in your list):**
```c
/* In CryptoCtx_t, the nonce counter is checked before EVERY encryption call */
CEEPEW_ASSERT(ctx->nonce_counter < CEEPEW_NONCE_HARD_LIMIT,
              CEEPEW_ERR_NONCE_EXHAUSTED);
ctx->nonce_counter++;  /* post-increment after assertion */
```

### 2.5 Layer 4 — Systematic Linear Code (Parity Check Matrix)

**This is the direct application of your linear algebra coursework (BAMAT205).**

**Code chosen:** Extended (15,11) Hamming code (single-error-correcting,
double-error-detecting, SECDED). Applied to the ciphertext in 11-bit chunks.

**Mathematical basis:**
- Generator matrix G (11×15): maps 11-bit data words → 15-bit codewords
- Parity check matrix H (4×15): H · c^T = 0 for valid codewords
- Syndrome s = H · r^T: if s ≠ 0, pinpoints the single flipped bit
- If syndrome indicates error in a check bit position, correct it
- If syndrome indicates uncorrectable pattern (double error), flag the packet

**Syndrome decoding (O(1), no loops — table lookup):**
```c
/* syndrome_to_bit_position[16] stored in flash as const uint8_t array       */
/* Index = 4-bit syndrome value. Value = bit position to flip (15=uncorrectable) */
static const uint8_t SYNDROME_TABLE[16] = {
    0,  /* 0000 = no error     */
    1,  /* 0001 = bit 0 error  */
    2,  /* 0010 = bit 1 error  */
    3,  /* 0011 = bit 2 error  */
    ...
    15  /* 1111 = uncorrectable */
};
```

**Why apply FEC to the ciphertext, not plaintext?**
Because bit errors occur at the transport layer (radio), AFTER encryption. Applying
FEC to ciphertext corrects radio-induced bit flips before Ascon's authentication tag
is checked. This prevents spurious authentication failures from radio noise.
The Ascon tag then catches any remaining integrity violations that are NOT radio noise
(i.e., active tampering) — these are fundamentally different threat classes.

**Overhead:** For a 140-byte ciphertext, the (15,11) code adds ~27% overhead
(140 × 15/11 ≈ 191 bytes). This is acceptable for the packet sizes involved.

**Struct:**
```c
typedef struct {
    uint8_t  codewords[CEEPEW_FEC_BUF_MAX]; /* encoded ciphertext + parity */
    uint16_t num_codewords;                  /* number of 15-bit codewords  */
    uint8_t  errors_corrected;               /* count of single-bit fixes   */
    bool     uncorrectable_error_detected;   /* double-error flag           */
} FecResult_t;
```

### 2.6 Layer 5 — CRC-32 Frame Check

**Purpose:** Rapid pre-filter. If the CRC-32 fails, discard the frame immediately
without invoking FEC or Ascon decryption. Saves significant computation on corrupt frames.

**Implementation:** ESP-IDF's `esp_crc32_le()` (hardware-accelerated on ESP32).
CRC computed over the entire frame (FEC-encoded ciphertext + header) and appended
as the last 4 bytes of the transmitted packet.

**Receive order:**
1. Check CRC-32 first (cheap, ~microseconds)
2. If CRC fails → discard, send NACK, done
3. If CRC passes → run FEC decoding
4. If FEC shows uncorrectable → discard, send NACK
5. Else → pass to Ascon decryption

### 2.7 Layer 6 — ARQ Protocol (Stop-and-Wait)

**Protocol:** Stop-and-Wait ARQ. Sender transmits one packet, then waits for ACK
or NACK before transmitting the next. Simple, verifiable, suited to semi/full duplex.

**Sequence numbering:** 8-bit rolling counter (0–255). Receiver tracks expected
sequence number; duplicates are detected and silently dropped.

**Retransmit policy:**
```c
#define CEEPEW_ARQ_MAX_RETRIES  3U   /* fixed upper bound on retransmit */
#define CEEPEW_ARQ_TIMEOUT_MS   500U /* per-attempt timeout             */

/* Loop is statically bounded: i < CEEPEW_ARQ_MAX_RETRIES always terminates */
for (uint8_t i = 0U; i < CEEPEW_ARQ_MAX_RETRIES; i++) {
    err = transport_send(frame, frame_len);
    CEEPEW_ASSERT(err == CEEPEW_OK, CEEPEW_ERR_TRANSPORT);

    ack = transport_wait_ack(CEEPEW_ARQ_TIMEOUT_MS);
    if (ack == ACK_RECEIVED) { break; }
    /* NACK or timeout: loop retransmits */
}
if (ack != ACK_RECEIVED) {
    return CEEPEW_ERR_MAX_RETRIES;
}
```

**Anti-replay (attack #11 in your list):** Receiver maintains a sliding window of
received sequence numbers (32-bit bitmap over last 32 sequence numbers). Any packet
with a sequence number already in the bitmap is a replay — discarded and logged.

### 2.8 Receive Pipeline (Full Stack, Top to Bottom)

```
[ESP-NOW frame received]
        │
        ▼
[CRC-32 check] ──FAIL──► [NACK + drop] ──► [Display "✗ CRC FAIL"]
        │ PASS
        ▼
[FEC decode — syndrome check per chunk]
        │         └──UNCORRECTABLE──► [NACK + drop] ──► [Display "✗ PKT DROP"]
        │ CORRECTABLE / CLEAN
        │         └──CORRECTED──► [Display "⚠ ECC FIX", log count]
        ▼
[Replay detection — sequence number bitmap check]
        │ NOT REPLAY
        ▼
[Ascon-128 authenticated decryption]
        │ TAG MISMATCH ──► [NACK + drop] ──► [Display "✗ AUTH FAIL"] ──► [Security alert]
        │ TAG OK
        ▼
[SHA-256 digest re-computation and comparison]
        │ MISMATCH ──► [Critical anomaly log] ──► [Session terminate]
        │ MATCH
        ▼
[Huffman decompress]
        ▼
[Bounds check on decompressed length]
        │ EXCEEDS MAX ──► [Drop, log]
        │ WITHIN BOUNDS
        ▼
[Ed25519 signature verify]
        │ FAIL ──► [Display "✗ SIG FAIL"] ──► [Session terminate]
        │ OK
        ▼
[Deliver plaintext to UI] ──► [Display "✓ MSG RX"]
        │
        ▼
[Schedule auto-wipe at T + TTL]
```

---

## 3. System-Wide Module Map

```
ceepew/
│
├── hal/                        Hardware Abstraction Layer
│   ├── hal_oled.h/.c           SSD1306 driver wrapper
│   ├── hal_input.h/.c          Potentiometer + button driver
│   ├── hal_radio.h/.c          ESP-NOW wrapper
│   ├── hal_rng.h/.c            ESP32 hardware RNG wrapper
│   └── hal_timer.h/.c          FreeRTOS timer abstractions
│
├── crypto/                     Cryptographic subsystem (Core 1)
│   ├── crypto_ascon.h/.c       Ascon-128 AEAD wrapper
│   ├── crypto_ecdh.h/.c        Curve25519 ECDH key exchange
│   ├── crypto_eddsa.h/.c       Ed25519 sign/verify
│   ├── crypto_hkdf.h/.c        HKDF-SHA256 key derivation
│   ├── crypto_sha256.h/.c      SHA-256 digest wrapper
│   ├── crypto_rng.h/.c         CSPRNG interface (wraps hal_rng)
│   └── crypto_ctx.h            CryptoCtx_t — master crypto state
│
├── ecc/                        Error Correction subsystem
│   ├── ecc_hamming.h/.c        (15,11) Hamming encode/decode
│   ├── ecc_crc32.h/.c          CRC-32 frame check
│   └── ecc_arq.h/.c            Stop-and-wait ARQ protocol
│
├── compress/                   Compression subsystem
│   ├── compress_huffman.h/.c   Static Huffman encode/decode
│   └── compress_table.h        Huffman code table (flash-resident)
│
├── transport/                  Transport subsystem
│   ├── transport_espnow.h/.c   ESP-NOW send/receive
│   ├── transport_packet.h      Packet struct definitions
│   └── transport_replay.h/.c   Sliding window replay detection
│
├── session/                    Session management
│   ├── session_fsm.h/.c        Main finite state machine
│   ├── session_discovery.h/.c  BLE advertisement + device sig
│   ├── session_pairing.h/.c    Session code exchange + ECDH
│   └── session_memory.h/.c     Message buffer + auto-wipe
│
├── ui/                         UI subsystem (Core 0)
│   ├── ui_fsm.h/.c             UI state machine
│   ├── ui_renderer.h/.c        OLED frame renderer
│   ├── ui_input.h/.c           Smoothed potentiometer + button
│   ├── ui_animator.h/.c        Animation frame sequencer
│   └── ui_feedback.h/.c        HX-63 style feedback panel
│
├── config/                     Compile-time configuration
│   ├── ceepew_config.h         All #define constants
│   └── ceepew_assert.h         Assertion macros + recovery
│
└── main.c                      Core 0/1 task launch
```

---

## 4. Module Specifications & Interfaces

### 4.1 ceepew_config.h — All Compile-Time Constants

```c
#ifndef CEEPEW_CONFIG_H
#define CEEPEW_CONFIG_H

/* ── Message Limits ────────────────────────────────────────────────────── */
#define CEEPEW_MAX_MSG_CHARS        160U   /* max plaintext chars           */
#define CEEPEW_MAX_MSG_BYTES        160U   /* 1 char = 1 byte (ASCII only)  */
#define CEEPEW_HUFF_BUF_MAX         200U   /* worst-case compressed bytes   */
#define CEEPEW_FEC_BUF_MAX          300U   /* worst-case FEC-encoded bytes  */
#define CEEPEW_PACKET_MAX_BYTES     512U   /* max total packet size         */

/* ── Crypto Parameters ─────────────────────────────────────────────────── */
#define CEEPEW_SESSION_KEY_BYTES    16U    /* Ascon-128 key = 128 bits      */
#define CEEPEW_NONCE_BYTES          16U    /* Ascon nonce = 128 bits        */
#define CEEPEW_TAG_BYTES            16U    /* Ascon auth tag = 128 bits     */
#define CEEPEW_SHA256_BYTES         32U    /* SHA-256 digest size           */
#define CEEPEW_ED25519_SIG_BYTES    64U    /* Ed25519 signature size        */
#define CEEPEW_ED25519_PK_BYTES     32U    /* Ed25519 public key size       */
#define CEEPEW_ED25519_SK_BYTES     64U    /* Ed25519 secret key size       */
#define CEEPEW_CURVE25519_PK_BYTES  32U    /* Curve25519 public key size    */
#define CEEPEW_CURVE25519_SK_BYTES  32U    /* Curve25519 secret key size    */
#define CEEPEW_DEVICE_SIG_BYTES     32U    /* HMAC-SHA256 device signature  */
#define CEEPEW_DEVICE_ID_BYTES      6U     /* ESP32 MAC address             */

/* ── Nonce Safety ──────────────────────────────────────────────────────── */
#define CEEPEW_NONCE_WARN_THRESHOLD 1000ULL        /* warn before exhaustion  */
#define CEEPEW_NONCE_HARD_LIMIT     (UINT64_MAX - CEEPEW_NONCE_WARN_THRESHOLD)

/* ── ARQ ────────────────────────────────────────────────────────────────── */
#define CEEPEW_ARQ_MAX_RETRIES      3U
#define CEEPEW_ARQ_TIMEOUT_MS       500U
#define CEEPEW_SEQ_WINDOW_SIZE      32U    /* replay window bits            */

/* ── Session ────────────────────────────────────────────────────────────── */
#define CEEPEW_SESSION_CODE_LEN     6U     /* alphanumeric pairing code     */
#define CEEPEW_PAIRING_TIMEOUT_S    120U   /* session code valid duration   */
#define CEEPEW_MSG_TTL_S            600U   /* 10-minute auto-wipe           */
#define CEEPEW_MAX_MESSAGES         20U    /* circular buffer depth         */

/* ── Input / UI ─────────────────────────────────────────────────────────── */
#define CEEPEW_POT_EMA_ALPHA_NUM    15U    /* EMA alpha = 15/100 = 0.15    */
#define CEEPEW_POT_EMA_ALPHA_DEN    100U
#define CEEPEW_POT_DEADZONE         40U    /* ADC units hysteresis band     */
#define CEEPEW_POT_EDGE_THRESHOLD   80U    /* ADC units from 0/4095         */
#define CEEPEW_EDGE_STABLE_MS       300U   /* time before auto-scroll fires */
#define CEEPEW_AUTOSCROLL_INTERVAL_MS 5000U
#define CEEPEW_CLICK_COOLDOWN_MS    200U

/* ── FEC ────────────────────────────────────────────────────────────────── */
#define CEEPEW_HAMMING_DATA_BITS    11U
#define CEEPEW_HAMMING_CODE_BITS    15U

/* ── FreeRTOS ───────────────────────────────────────────────────────────── */
#define CEEPEW_CORE0_STACK_BYTES    4096U
#define CEEPEW_CORE1_STACK_BYTES    8192U  /* crypto needs more stack       */
#define CEEPEW_QUEUE_DEPTH          8U

#endif /* CEEPEW_CONFIG_H */
```

### 4.2 ceepew_assert.h — Assertion & Error Framework

```c
#ifndef CEEPEW_ASSERT_H
#define CEEPEW_ASSERT_H

#include <stdbool.h>
#include <stdint.h>

/* ── Error Codes ─────────────────────────────────────────────────────────── */
typedef enum {
    CEEPEW_OK                   = 0,
    CEEPEW_ERR_NULL_PTR         = 1,
    CEEPEW_ERR_BOUNDS           = 2,
    CEEPEW_ERR_CRYPTO           = 3,
    CEEPEW_ERR_NONCE_EXHAUSTED  = 4,
    CEEPEW_ERR_AUTH_FAIL        = 5,
    CEEPEW_ERR_SIG_FAIL         = 6,
    CEEPEW_ERR_FEC_UNCORRECT    = 7,
    CEEPEW_ERR_CRC_FAIL         = 8,
    CEEPEW_ERR_REPLAY           = 9,
    CEEPEW_ERR_TRANSPORT        = 10,
    CEEPEW_ERR_MAX_RETRIES      = 11,
    CEEPEW_ERR_SESSION          = 12,
    CEEPEW_ERR_COMPRESS         = 13,
    CEEPEW_ERR_PARAM            = 14,
    CEEPEW_ERR_WIPE             = 15,
    CEEPEW_ERR_ANOMALY          = 16,   /* SHA vs Ascon mismatch = critical */
} CeePewErr_t;

/*
 * CEEPEW_ASSERT(condition, error_code)
 *
 * Rules:
 *  - condition must be side-effect free (boolean expression only)
 *  - On failure: logs the failure location and returns error_code to caller
 *  - Every function must have >= 2 assertions
 *  - Static checkers must be able to evaluate the assertion
 *
 * The macro expands to a complete syntactic unit (do-while block).
 */
#define CEEPEW_ASSERT(condition, err_code)              \
    do {                                                \
        if (!(condition)) {                             \
            ceepew_log_assert(__FILE__, __LINE__,       \
                              #condition, (err_code));  \
            return (err_code);                          \
        }                                               \
    } while (0)

/* Void-function variant: logs and returns (no value) */
#define CEEPEW_ASSERT_VOID(condition)                   \
    do {                                                \
        if (!(condition)) {                             \
            ceepew_log_assert(__FILE__, __LINE__,       \
                              #condition, CEEPEW_ERR_PARAM); \
            return;                                     \
        }                                               \
    } while (0)

void ceepew_log_assert(const char *file, uint32_t line,
                       const char *expr, CeePewErr_t code);

#endif /* CEEPEW_ASSERT_H */
```

### 4.3 CryptoCtx_t — Master Crypto State Struct

```c
/* crypto/crypto_ctx.h */
#ifndef CRYPTO_CTX_H
#define CRYPTO_CTX_H

#include "ceepew_config.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * CryptoCtx_t — all cryptographic state for one session.
 * Allocated ONCE at startup in static storage. NEVER heap-allocated.
 * Zeroed (memset to 0) on session end via crypto_ctx_destroy().
 */
typedef struct {
    /* Session symmetric key (Ascon-128) */
    uint8_t  session_key[CEEPEW_SESSION_KEY_BYTES];

    /* Nonce counter — monotonically increasing, NEVER reused */
    uint64_t nonce_counter;

    /* Session identity (first 8 bytes of HKDF output, used in nonce) */
    uint64_t session_id;

    /* Ed25519 signing keypair (our keys) */
    uint8_t  sign_sk[CEEPEW_ED25519_SK_BYTES];
    uint8_t  sign_pk[CEEPEW_ED25519_PK_BYTES];

    /* Peer's Ed25519 verification key */
    uint8_t  peer_sign_pk[CEEPEW_ED25519_PK_BYTES];

    /* Ephemeral Curve25519 keypair (used during key exchange, cleared after) */
    uint8_t  ecdh_sk[CEEPEW_CURVE25519_SK_BYTES];
    uint8_t  ecdh_pk[CEEPEW_CURVE25519_PK_BYTES];

    /* Device identity (HMAC-SHA256 of device_secret + protocol string) */
    uint8_t  device_sig[CEEPEW_DEVICE_SIG_BYTES];
    uint8_t  device_id[CEEPEW_DEVICE_ID_BYTES];   /* MAC address */

    /* State flags */
    bool     session_active;
    bool     keys_derived;
    bool     ecdh_keys_cleared;   /* true after ECDH keys are wiped */
} CryptoCtx_t;

/* Single static instance — no dynamic allocation */
extern CryptoCtx_t g_crypto_ctx;

CeePewErr_t crypto_ctx_init(CryptoCtx_t *ctx);
void        crypto_ctx_destroy(CryptoCtx_t *ctx);  /* zeroes all key material */

#endif /* CRYPTO_CTX_H */
```

### 4.4 MessageBuffer_t — Static Message Storage

```c
/* session/session_memory.h */
typedef struct {
    uint8_t  plaintext[CEEPEW_MAX_MSG_BYTES];
    uint16_t length;
    uint32_t timestamp_s;       /* unix epoch at receipt/send    */
    uint32_t wipe_at_s;         /* timestamp_s + CEEPEW_MSG_TTL_S */
    bool     is_outgoing;       /* true = sent, false = received */
    bool     occupied;          /* slot in use                   */
} MessageSlot_t;

typedef struct {
    MessageSlot_t slots[CEEPEW_MAX_MESSAGES];  /* static circular buffer    */
    uint8_t       write_idx;                   /* next write position       */
    uint8_t       count;                        /* current occupancy         */
} MessageBuffer_t;

extern MessageBuffer_t g_message_buffer;

CeePewErr_t  msgbuf_insert(MessageBuffer_t *buf, const uint8_t *plaintext,
                            uint16_t len, bool outgoing, uint32_t now_s);
void         msgbuf_wipe_expired(MessageBuffer_t *buf, uint32_t now_s);
void         msgbuf_wipe_all(MessageBuffer_t *buf);
```

### 4.5 InputCtx_t — Potentiometer State

```c
/* ui/ui_input.h */
typedef struct {
    uint16_t raw_adc;           /* latest raw ADC reading (0–4095)      */
    uint16_t smoothed_adc;      /* EMA-smoothed value                   */
    uint16_t last_confirmed;    /* ADC value at last confirmed selection */
    uint8_t  cursor_pos;        /* current character index in alphabet  */
    bool     at_left_edge;      /* smoothed near 0                      */
    bool     at_right_edge;     /* smoothed near 4095                   */
    uint32_t edge_stable_since; /* millis() when edge was first detected */
    uint32_t last_click_ms;     /* millis() of last button press        */
    bool     click_pending;     /* debounced click event flag           */
} InputCtx_t;

CeePewErr_t input_update(InputCtx_t *ctx, uint16_t raw_adc,
                          bool raw_button, uint32_t now_ms);
uint8_t     input_get_char_index(const InputCtx_t *ctx);
bool        input_consume_click(InputCtx_t *ctx);
```

---

## 5. Data Structures & Memory Layout

### 5.1 Packet Wire Format

```
[ CEEPEW Packet — total ≤ CEEPEW_PACKET_MAX_BYTES ]

Offset  Size  Field
──────  ────  ──────────────────────────────────────────────────────
0       1     version         (uint8_t, always 0x01)
1       6     sender_id       (device MAC address)
7       1     seq_num         (uint8_t, ARQ sequence number)
8       4     timestamp       (uint32_t, Unix epoch, little-endian)
12      1     flags           (bit 0: FEC_CORRECTED, bit 1: COMPRESSED)
13      2     payload_len     (uint16_t, bytes of FEC-encoded ciphertext)
15      N     fec_ciphertext  (N = payload_len, FEC-encoded Ascon output)
15+N    16    ascon_tag       (128-bit Ascon authentication tag)
31+N    64    ed25519_sig     (Ed25519 signature over bytes 0 to 30+N)
95+N    4     crc32           (CRC-32 over bytes 0 to 94+N)
```

**Associated Data for Ascon:** bytes 0–14 (the header). This cryptographically
binds the header to the ciphertext — any header modification is caught by the auth tag.

### 5.2 Key Derivation Tree

```
ECDH(our_ecdh_sk, peer_ecdh_pk)
    └── shared_secret (32 bytes, Curve25519 output)
            │
            ▼
        HKDF-SHA256
            │   salt    = SHA256(session_code)
            │   info    = "CEEPEW_SESSION_v1" || timestamp_string
            │   length  = 64 bytes total
            │
            ├── [0:15]  → session_key     (Ascon-128 key)
            ├── [16:23] → session_id      (8 bytes, upper half of nonce)
            └── [24:63] → reserved for future rekeying

After HKDF:
    ecdh_sk, ecdh_pk → zeroed (volatile memset + compiler barrier)
```

### 5.3 Memory Budget (ESP32 SRAM = 520 KB)

| Region | Size | Notes |
|---|---|---|
| g_crypto_ctx | ~384 bytes | all key material |
| g_message_buffer | ~3,340 bytes | 20 slots × 160B + metadata |
| FEC encode/decode bufs | 600 bytes | two static buffers |
| Huffman encode/decode bufs | 400 bytes | two static buffers |
| Packet TX/RX buffers | 1,024 bytes | two × 512B |
| UI frame buffer | 1,024 bytes | SSD1306 display buffer |
| FreeRTOS stacks | ~12,288 bytes | two tasks |
| **Total** | **~19 KB** | **<4% of SRAM** — safe margin |

**No OS heap allocation after init.** `malloc` / `new` / `std::vector` are forbidden.
Variable-length processing uses `region_alloc(&g_region, size)` from the 200 KB
static region pool (see Addendum D). Every `region_alloc` return value must be
NULL-checked. The pool is secure-zeroed at session end via `region_reset()`.

---

## 6. State Machine Specification

### 6.1 Session FSM

```
States:
    ST_BOOT
    ST_DISCOVERY
    ST_PAIRING
    ST_KEY_EXCHANGE
    ST_SECURE_CHAT
    ST_ERROR
    ST_WIPE

Transitions:
    ST_BOOT         → ST_DISCOVERY      on: init complete
    ST_DISCOVERY    → ST_PAIRING        on: peer CEE-PEW device signature verified
    ST_DISCOVERY    → ST_ERROR          on: timeout (CEEPEW_PAIRING_TIMEOUT_S)
    ST_PAIRING      → ST_KEY_EXCHANGE   on: session codes match (local check)
    ST_PAIRING      → ST_DISCOVERY      on: code mismatch or timeout
    ST_KEY_EXCHANGE → ST_SECURE_CHAT    on: ECDH complete, keys derived, ACK exchanged
    ST_KEY_EXCHANGE → ST_ERROR          on: ECDH failure, auth failure
    ST_SECURE_CHAT  → ST_WIPE           on: session end OR nonce exhaustion
    ST_SECURE_CHAT  → ST_ERROR          on: repeated auth failure (>3 in 60s)
    ST_ERROR        → ST_WIPE           on: always (no recovery, must wipe)
    ST_WIPE         → ST_BOOT           on: wipe complete
```

### 6.2 UI FSM (Core 0) — Mirrors Session FSM

Each session state has a corresponding UI state that drives the OLED renderer.
The UI FSM receives events from a FreeRTOS queue fed by Core 1.

---

## 7. Attack Resistance Mapping

| Attack | CEE-PEW Defence | Layer |
|---|---|---|
| **Cryptanalytic attacks** | Ascon-128 (NIST standard, full security margin) | Crypto |
| **Differential cryptanalysis** | Ascon's sponge structure; no S-box lookup tables | Crypto |
| **Linear cryptanalysis** | Ascon permutation designed to resist; Ed25519 for auth | Crypto |
| **Meet-in-the-middle** | 128-bit key → 2^64 meet-in-middle threshold far above feasibility | Crypto |
| **Algebraic attacks** | Curve25519's prime-order group, Ascon's non-algebraic structure | Crypto |
| **Cache-timing attacks** | Ascon & Curve25519 are constant-time implementations (no secret-dependent branches or table lookups). Speck's ARX structure also immune to cache timing | Crypto |
| **Rowhammer** | ESP32 uses SRAM (not DRAM). Rowhammer is a DRAM-specific attack; not applicable | HW |
| **Padding oracle** | Ascon-128 is an AEAD — no padding scheme exists. Tag verify is constant-time; no oracle possible | Crypto |
| **Chosen plaintext** | Ascon with random nonce per message; ciphertext reveals nothing about plaintext structure | Crypto |
| **Chosen ciphertext** | Ascon authentication tag verification fails before any decryption oracle is possible | Crypto |
| **Buffer overflow** | All buffers statically sized, bounds checked via CEEPEW_ASSERT before every access. No heap = no heap overflow | Memory |
| **RNG failure** | Hardware RNG (ESP32 thermal noise). ASSERT on zero output. Session init fails if RNG returns deterministic sequence | Crypto |
| **Key reuse / nonce reuse** | nonce_counter strictly monotone; ASSERT before every encryption; hard limit enforced; session_id unique per session | Crypto |
| **Poor key management** | Single CryptoCtx_t; ECDH keys zeroed immediately after HKDF; session_key zeroed on session end; no key duplication in code | Crypto |
| **MITM attack** | Session code as pre-shared OOB trust anchor; ECDH output bound to session_code via HKDF salt; display key fingerprint for manual verification; Ed25519 device signatures | Protocol |
| **Replay attack** | 64-bit nonce counter + 32-bit sliding window bitmap replay detection + sequence number ARQ | Transport |
| **Side-channel (power analysis)** | Constant-time crypto implementations; no branching on secret data. Noted as partial mitigation — dedicated hardware countermeasures beyond scope of v1 | Crypto |

---

## 8. Coding Standards (JPL Power of 10 — Adapted)

These rules are **mandatory from day one of development**.

### Rule 1 — Fixed Loop Bounds
Every loop must have a statically provable upper bound. The loop counter must be
compared against a compile-time constant or a parameter that has been bounds-checked
before the loop. Iterators over arrays must use the array's compile-time size constant.

```c
/* CORRECT: bound is CEEPEW_MAX_MESSAGES — a compile-time constant */
for (uint8_t i = 0U; i < CEEPEW_MAX_MESSAGES; i++) { ... }

/* WRONG: bound is a runtime variable without prior validation */
for (uint8_t i = 0U; i < user_len; i++) { ... }  /* VIOLATION */

/* CORRECT: validate first, then loop */
CEEPEW_ASSERT(user_len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);
for (uint16_t i = 0U; i < user_len; i++) { ... }
```

### Rule 2 — No Dynamic Allocation After Init

```c
/* FORBIDDEN anywhere after app_main setup phase: */
malloc(), calloc(), realloc(), free()
new, delete (C++)
std::vector, std::string, std::map (heap-backed STL)

/* ALLOWED: */
static uint8_t buf[CEEPEW_PACKET_MAX_BYTES];   /* module-level static */
uint8_t local[32];                              /* stack allocation    */
```

All static buffers that hold key material MUST be declared `volatile` to prevent
compiler optimisation from eliding zero-writes during destruction:
```c
static volatile uint8_t session_key[CEEPEW_SESSION_KEY_BYTES];
```

### Rule 3 — Minimum 2 Assertions Per Function

Every non-trivial function must contain at least two `CEEPEW_ASSERT` calls.
The assertions must check for conditions that are genuinely impossible in correct
execution (not just defensive input validation, which is a separate requirement).

```c
CeePewErr_t crypto_encrypt(CryptoCtx_t *ctx, const uint8_t *plaintext,
                            uint16_t pt_len, uint8_t *ciphertext_out)
{
    /* Assertion 1: context must be initialised and session active */
    CEEPEW_ASSERT(ctx != NULL && ctx->session_active, CEEPEW_ERR_NULL_PTR);

    /* Assertion 2: nonce counter must not have wrapped (invariant) */
    CEEPEW_ASSERT(ctx->nonce_counter < CEEPEW_NONCE_HARD_LIMIT,
                  CEEPEW_ERR_NONCE_EXHAUSTED);

    /* Input validation (separate from assertions) */
    if (plaintext == NULL || ciphertext_out == NULL) {
        return CEEPEW_ERR_NULL_PTR;
    }
    if (pt_len == 0U || pt_len > CEEPEW_MAX_MSG_BYTES) {
        return CEEPEW_ERR_BOUNDS;
    }

    /* ... encryption logic ... */

    /* Assertion 3 (bonus): nonce was incremented */
    CEEPEW_ASSERT(ctx->nonce_counter > 0U, CEEPEW_ERR_ANOMALY);

    return CEEPEW_OK;
}
```

### Rule 4 — Check All Return Values

```c
/* WRONG: ignoring return value */
crypto_encrypt(ctx, pt, len, ct);

/* CORRECT: check and propagate */
CeePewErr_t err = crypto_encrypt(ctx, pt, len, ct);
if (err != CEEPEW_OK) { return err; }

/* For void functions that cannot fail: document why return check is not needed */
```

### Rule 5 — Smallest Possible Scope

Variables are declared at the innermost scope where they are first used. No
module-global mutable variables except the explicitly designated global instances
(`g_crypto_ctx`, `g_message_buffer`). All other state is passed as parameters.

### Rule 6 — Preprocessor Discipline

```c
/* ALLOWED: object-like macros for constants */
#define CEEPEW_MAX_MSG_BYTES 160U

/* ALLOWED: function-like macros that expand to complete syntactic units */
#define CEEPEW_ASSERT(cond, err) do { if (!(cond)) { ... } } while(0)

/* FORBIDDEN: macros that expand to partial expressions */
#define HALF_EXPR(x)  (x +     /* VIOLATION: incomplete expression */

/* FORBIDDEN: recursive macros */
/* FORBIDDEN: variadic macros (...) unless absolutely necessary and documented */

/* MINIMISE #ifdef: use only for platform guards and debug toggles */
```

### Rule 7 — Compiler Warnings as Errors

Required compiler flags (GCC/Xtensa toolchain):
```
-Wall -Wextra -Werror -Wpedantic
-Wformat=2 -Wformat-security
-Wnull-dereference
-Wstrict-prototypes
-Wmissing-prototypes
-Wcast-align
-Wconversion
-Wshadow
-Wundef
-fstack-protector-strong
```

### Rule 8 — Constant-Time Requirements for Crypto Operations

Any comparison involving secret data (keys, tags, hashes) MUST use constant-time
comparison to resist timing side-channels (attack #6 in your list):

```c
/* FORBIDDEN for secret data: */
if (memcmp(tag_a, tag_b, 16) == 0) { ... }   /* timing leak */

/* REQUIRED: constant-time comparison */
static bool crypto_ct_equal(const uint8_t *a, const uint8_t *b, uint16_t len)
{
    uint8_t diff = 0U;
    CEEPEW_ASSERT(a != NULL, false);
    CEEPEW_ASSERT(b != NULL, false);
    /* Loop bound: len is validated before call, <= CEEPEW_TAG_BYTES */
    for (uint16_t i = 0U; i < len; i++) {
        diff |= (a[i] ^ b[i]);
    }
    return (diff == 0U);
}
```

### Rule 9 — Volatile Zeroing of Key Material

```c
/* Prevents compiler from optimising away security-critical zeroes */
static void secure_zero(volatile void *ptr, uint32_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    CEEPEW_ASSERT(p != NULL, /* void fn: */ );
    CEEPEW_ASSERT(len <= 512U, /* void fn: */ );
    for (uint32_t i = 0U; i < len; i++) {
        p[i] = 0U;
    }
    /* Memory barrier: prevents reordering past this point */
    __asm__ __volatile__("" ::: "memory");
}
```

---

## 9. Build & Static Analysis Pipeline

### 9.1 Daily Checks (mandatory before any commit)

```bash
# 1. Build with all warnings as errors
idf.py build 2>&1 | grep -E "warning:|error:" && echo "BUILD CLEAN"

# 2. cppcheck static analysis
cppcheck --enable=all --error-exitcode=1 \
         --suppress=missingIncludeSystem \
         --std=c11 firmware/

# 3. clang-tidy (if Xtensa LLVM toolchain available)
clang-tidy firmware/**/*.c -- -Ifirmware/config -Ifirmware/include

# 4. pc-lint or Polyspace (if available, recommended for certification-grade)
```

### 9.2 CMakeLists / idf_component.cmake Structure

Each subdirectory (`crypto/`, `ecc/`, `ui/`, etc.) is a separate IDF component
with its own `CMakeLists.txt`. Dependencies are explicit and unidirectional:
```
ui      → hal, session (no dependency on crypto directly)
session → crypto, transport, compress
crypto  → hal_rng only
ecc     → (no dependencies on other ceepew modules)
transport → hal_radio, ecc
```
This enforces modular separation and prevents circular dependencies.

---

## Summary: What to Build First (Ordered Milestones)

| Milestone | Target | Test Criteria |
|---|---|---|
| M0 | `ceepew_config.h` + `ceepew_assert.h` complete | Compiles clean, zero warnings |
| M1 | `hal_input` — potentiometer + EMA filter | Cursor stable on OLED, no bounce |
| M2 | `hal_oled` — basic text + animation frames | HX-63 boot screen renders correctly |
| M3 | `ecc_hamming` — encode/decode | 1000 random vectors: inject 1-bit error, all corrected |
| M4 | `ecc_crc32` + `ecc_arq` | Simulated packet loss, 3-retry recovery works |
| M5 | `crypto_rng` + `crypto_sha256` | SHA-256 output matches known test vectors |
| M6 | `crypto_ecdh` — Curve25519 | Key agreement produces identical shared secret both sides |
| M7 | `crypto_ascon` — Ascon-128 | NIST test vectors pass; nonce counter monotone |
| M8 | `crypto_eddsa` — Ed25519 | Sign/verify round trip; tampered message rejected |
| M9 | `crypto_hkdf` — HKDF-SHA256 | Output matches RFC 5869 test vectors |
| M10 | `compress_huffman` | Compress/decompress round trip; incompressible passthrough |
| M11 | `transport_espnow` — basic P2P | Two devices exchange packets; RSSI visible |
| M12 | `session_fsm` — full pairing flow | Two devices pair via session code and derive matching keys |
| M13 | Full integration | End-to-end encrypted message delivered; all UI states render |
| M14 | Auto-wipe + HX-63 feedback panel | Messages disappear at TTL; security panel correct |

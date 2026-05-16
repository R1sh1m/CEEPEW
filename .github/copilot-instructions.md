# CEE-PEW — GitHub Copilot Project Instructions
# Cryptographic End-to-End Peer-to-peer Encrypted Wireless Communicator
# Author: Rishi Misra (25BCE2454), VIT Vellore
# Hardware: ESP32-WROOM-32 | Framework: ESP-IDF v6.0.1

---

## IDENTITY

You are the principal engineer for CEE-PEW. Every suggestion you make
must be a complete, production-grade, deployable implementation.

**NEVER:**
- Generate stub functions with `/* TODO */` bodies
- Generate `return CEEPEW_OK; // placeholder`
- Generate `// implement this` comments instead of code
- Generate wrapper functions that just call a single inner function
- Omit error handling with "for brevity"
- Suggest pseudocode or skeleton code

**ALWAYS:**
- Implement the full algorithm, not a description of it
- Include every assertion, every bounds check, every return value check
- Write code that compiles and runs correctly on first attempt
- Match the complexity of the real algorithm being implemented

---

## HARDWARE CONTEXT (LOCKED — DO NOT MODIFY)

```
MCU:      ESP32-WROOM-32, dual-core Xtensa LX6, 240MHz
SRAM:     520 KB total
Flash:    4 MB
Display:  SSD1306 OLED 128×64, I2C addr 0x3C (fallback 0x3D)
Input 1:  B10K rotary potentiometer, GPIO 34 (ADC1_CH6)
Input 2:  Click button, GPIO 35 (SPST-NO, INPUT_PULLUP, active LOW)
Input 3:  Push-lock switch, GPIO 5 (INPUT_PULLUP, active LOW = DIAG)
RGB LED:  Common-cathode; R=GPIO2, G=GPIO18, B=GPIO23
Radio:    ESP32 internal (BLE for discovery/pairing, ESP-NOW for chat)
OS:       FreeRTOS (ESP-IDF built-in)
Framework: ESP-IDF v6.0.1 — NOT Arduino
Build:    CMake + idf.py
```

---

## CRYPTOGRAPHIC STACK (IMPLEMENT IN FULL — NO SHORTCUTS)

```
Layer 1: TweetNaCl crypto_box (Curve25519 ECDH + XSalsa20 + Poly1305)
Layer 2: Ascon-128 AEAD outer envelope (NIST SP 800-232)
Layer 3: SHA-256 plaintext digest (mbedtls, HW-accelerated)
Layer 4: Ed25519 per-session signing
Layer 5: HKDF-SHA256 key derivation with digital_sum salt
Layer 6: Session-permuted (15,11) Hamming FEC on ciphertext
Layer 7: CRC-32 + Stop-and-Wait ARQ with WireGuard replay window
Layer 8: ESP-NOW with PRG channel hopping
```

**Key derivation (implement exactly):**
```
salt  = SHA256(digital_sum_mix(code) || code)
info  = "CEEPEW_SESSION_v1" || id_A || id_B || commitment || t_round_bytes
HKDF output [0:15]  → Ascon-128 session key
HKDF output [16:47] → crypto_box seed
HKDF output [48:55] → session_id (nonce upper 64 bits)
```

---

## CODING STANDARDS — ENFORCED ON EVERY SUGGESTION

### 1. LOOP BOUNDS
Every loop must have a statically provable termination bound.
```c
/* CORRECT — compile-time constant bound */
for (uint8_t i = 0U; i < CEEPEW_MAX_MESSAGES; i++) { ... }

/* CORRECT — runtime value validated before loop */
CEEPEW_ASSERT_BOUND(len, CEEPEW_REGION_POOL_BYTES);
for (uint32_t i = 0U; i < len; i++) { ... }

/* WRONG — reject this pattern */
for (int i = 0; i < n; i++) { ... }  // n unchecked
```

### 2. NO DYNAMIC ALLOCATION
```c
/* FORBIDDEN — never suggest these */
malloc()  calloc()  realloc()  free()
new  delete  std::vector  std::string

/* CORRECT — use region allocator */
uint8_t *buf = region_alloc(&g_region, size);
CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_BOUNDS);
```

### 3. MINIMUM 2 ASSERTIONS PER FUNCTION
```c
CeePewErr_t example_fn(CryptoCtx_t *ctx, const uint8_t *data, uint16_t len)
{
    CEEPEW_ASSERT(ctx != NULL && ctx->session_active, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(data != NULL && len > 0U && len <= CEEPEW_MAX_MSG_BYTES,
                  CEEPEW_ERR_BOUNDS);
    /* ... */
    return CEEPEW_OK;
}
```

### 4. CHECK ALL RETURN VALUES
```c
/* WRONG */
crypto_encrypt(ctx, pt, len, ct);

/* CORRECT */
CeePewErr_t err = crypto_encrypt(ctx, pt, len, ct);
if (err != CEEPEW_OK) { return err; }
```

### 5. CONSTANT-TIME FOR ALL SECRET COMPARISONS
```c
/* FORBIDDEN on secret data */
if (memcmp(tag_a, tag_b, 16) == 0)  // timing leak

/* REQUIRED */
if (crypto_ct_equal(tag_a, tag_b, 16U))  // constant-time
```

### 6. SECURE ZEROING OF KEY MATERIAL
```c
/* REQUIRED pattern — compiler cannot elide volatile writes */
static void secure_zero(volatile void *ptr, uint32_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    for (uint32_t i = 0U; i < len; i++) { p[i] = 0U; }
    __asm__ __volatile__("" ::: "memory");
}
```

### 7. PACKED WIRE STRUCTS
```c
/* ALL packet/frame structs must have this attribute */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  sender_id[6];
    uint8_t  seq_num;
    uint32_t timestamp;
} CeePewPacketHeader_t;
```

### 8. C11 ONLY
```c
/* FORBIDDEN */
delay()  millis()  analogRead()  Serial.print()  String

/* CORRECT ESP-IDF equivalents */
vTaskDelay(pdMS_TO_TICKS(ms))
esp_timer_get_time()
adc_oneshot_read()
ESP_LOGI() / CEEPEW_LOG()
```

### 9. GPIO NUMBERS — ONLY IN hal_pins.h
```c
/* FORBIDDEN in any file except ceepew_hal/hal_pins.h */
gpio_set_level(GPIO_NUM_2, 1);

/* CORRECT */
gpio_set_level(CEEPEW_PIN_RGB_RED, 1);
```

### 10. RGB LED — ONLY THROUGH hal_rgb API
```c
/* FORBIDDEN everywhere except ceepew_hal/hal_rgb.c */
gpio_set_level(GPIO_NUM_2, 1);

/* CORRECT — from any other module */
rgb_set_pattern(RGB_SECURE);
```

---

## ALGORITHM IMPLEMENTATION REQUIREMENTS

When implementing any of the following, use the REAL algorithm:

### Hamming (15,11) FEC
- Implement the actual generator matrix G and parity check matrix H
- Real syndrome decoding via lookup table (not a placeholder switch)
- Session-permuted columns: derive permutation from session_key[0:15]
- Fisher-Yates shuffle seeded by session key XOR hop counter

### Huffman Compression
- Static code table trained on English letter frequencies
- Actual bit-packing: LSB-first bit stream
- Escape sequence for symbols not in the primary table
- Passthrough flag when compressed >= uncompressed

### Digital Sum (Rishi's algorithm)
- Iterative digit root using the 9-elimination method
- 32-byte mixing output via sliding 9-byte windows + XOR folding
- Used as HKDF salt preprocessor: SHA256(digital_sum_mix(code) || code)

### HKDF-SHA256
- Full RFC 5869 implementation using mbedtls primitives
- Extract phase: HMAC-SHA256(salt, IKM)
- Expand phase: T(1) || T(2) || ... until output length reached
- Bind all session parameters in the info field

### Ascon-128
- Full sponge permutation (12 rounds for initialization/finalization, 6 for squeeze)
- Proper domain separation constants
- Associated data processing with padding
- Tag verification must be constant-time

### WireGuard Anti-Replay Window
- 64-bit sliding bitmap, not a simple array
- Exact WireGuard algorithm: advance window on new max, check bitmap within window
- Reject silently — no NACK on auth/replay failure

### PKCS7 Padding
- Constant-time unpadding verification (timing-safe)
- Pad to 64-byte boundary (XSalsa20 block size)

### CRC-32
- Use esp_crc32_le() hardware acceleration
- Computed over full frame including header

### Stop-and-Wait ARQ
- Exactly CEEPEW_ARQ_MAX_RETRIES = 3 retries
- CEEPEW_ARQ_TIMEOUT_MS = 500ms per attempt
- 8-bit rolling sequence number with duplicate detection

---

## MODULE DEPENDENCY RULES

```
ui          → ceepew_hal (read-only), session queues only
session     → crypto, transport, compress, mem, tools
crypto      → ceepew_hal (rng only), mbedtls
ecc         → no ceepew dependencies
transport   → ceepew_hal, crypto, ecc, mem
compress    → no ceepew dependencies
mem         → no ceepew dependencies
tools       → mem
ceepew_hal  → ESP-IDF only
```

Cross-layer includes are architecture violations. If a feature seems
to require one, use a FreeRTOS queue instead.

---

## RESPONSE FORMAT FOR CODE

Every code response must follow this exact format:

```c
/* component/filename.c */

#include "filename.h"
#include "ceepew_assert.h"
/* ... other includes ... */

/* Design note: [one paragraph explaining WHY this implementation
   is structured as it is, not WHAT it does] */

CeePewErr_t function_name(/* full typed params */)
{
    CEEPEW_ASSERT(/* condition 1 */, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(/* condition 2 */, CEEPEW_ERR_BOUNDS);

    /* full implementation — no TODOs, no placeholders */

    return CEEPEW_OK;
}
```

And always provide the matching `.h` declaration when introducing a
new function.

---

## SECURITY — CHECK EVERY SUGGESTION AGAINST THIS

| Attack | Required mitigation |
|--------|-------------------|
| Nonce reuse | CEEPEW_ASSERT(nonce_counter < CEEPEW_NONCE_HARD_LIMIT) before every encrypt |
| Padding oracle | Constant-time unpadding; Ascon tag verified before any decryption output |
| Timing side-channel | crypto_ct_equal() for all secret comparisons; no secret-dependent branches |
| Buffer overflow | CEEPEW_ASSERT bounds before every write; static buffers only |
| Key material leak | volatile key buffers; secure_zero() + barrier on session end |
| Replay | WireGuard 64-bit bitmap + timestamp ±15s + nonce counter |
| MITM | Session code in HKDF salt; fingerprint display; Ed25519 device signatures |

---

## WHAT CONSTITUTES A PLACEHOLDER (REJECT AND REGENERATE)

These patterns indicate a placeholder — regenerate until they are gone:

```c
// TODO: implement
/* placeholder */
return CEEPEW_OK; /* stub */
(void)param;      /* suppress unused warning in stub */
// For now, just...
// Simplified version...
// In production, this would...
memset(output, 0, len); // placeholder encryption
```

A real implementation of crypto_box does not look like:
```c
// Apply XSalsa20 stream cipher (placeholder)
memcpy(ciphertext, plaintext, len);
```

It looks like the actual Salsa20 quarter-round, key schedule, and
counter-mode XOR with the plaintext.

---

## CURRENT PROJECT STATE

Architecture documents:
- CEEPEW_Architecture_v1.md
- CEEPEW_Addendum_A_PinConfig_ResourceMonitor.md
- CEEPEW_Addendum_B_Numerology_and_ProjectMode.md
- CEEPEW_Addendum_C_ESL_Security_Framework.md
- CEEPEW_Addendum_D_Arbitrary_Computation.md
- CEEPEW_Final_Master_Specification_v2.md (highest authority)

Milestone order (implement in sequence):
M0  → ceepew_config.h + ceepew_assert.h + hal_pins.h
M1  → hal_adc, hal_input (EMA filter, DIAG switch)
M2  → hal_oled (SSD1306, dual-address init)
M3  → hal_rgb (pattern engine, FreeRTOS timer)
M4  → ecc_hamming (session-permuted G/H) + ecc_crc32
M5  → ecc_arq + transport_replay (WireGuard bitmap)
M6  → compress_huffman (static table, real bit-packing)
M7  → tools/digital_sum (Rishi's algorithm, real implementation)
M8  → crypto_rng + crypto_sha256 + crypto_hkdf (RFC 5869)
M9  → crypto_ecdh (Curve25519 via TweetNaCl)
M10 → crypto_box_wrap (NaCl zero-pad, static buffers)
M11 → crypto_ascon (full Ascon-128 sponge)
M12 → crypto_eddsa (Ed25519 sign/verify)
M13 → transport_ble (Phase 1+2)
M14 → transport_espnow + transport_esl + transport_hop
M15 → session_fsm (3-phase pairing)
M16-M21 → UI phases, DIAG, integration

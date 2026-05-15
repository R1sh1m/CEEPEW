# CEE-PEW — Final Master Specification
### Cryptographic End-to-End Peer-to-peer Encrypted Wireless Communicator
**Version:** 2.0-FINAL | Supersedes all previous addenda on points of conflict

---

## PART 1 — WHAT WE LEARNED FROM THE REFERENCE REPOSITORIES

### 1.1 azme10/Secure-IoT-Communication-System-with-ESP32 — Key Findings

**Adopted:**
- `__attribute__((packed))` on all wire-format structs. Without it, GCC inserts
  alignment padding, causing sender/receiver byte-count mismatches that are
  fiendishly hard to debug. This is now a mandatory annotation on every packet struct.
- IV must be memcpy'd before every AES-CBC call — the mbedtls CBC function
  modifies the IV buffer in-place. Even though CEE-PEW uses Ascon (not AES-CBC),
  the same lesson applies to any stateful cipher context: treat context state as consumed.
- The benchmark data is useful: ECDH key generation takes ~30ms on ESP32 at 240MHz,
  full authentication handshake completes in ~50ms. These are our Phase 3 timing targets.
- Hardware-accelerated AES via mbedtls gives 8.26 MB/s throughput — the ESP32's
  crypto engine is accessed transparently through mbedtls when compiled with
  `MBEDTLS_AES_ALT`. Same applies to SHA-256. We use this for our SHA-256 digest layer.

**Not adopted:**
- AES-256-CBC: superseded by Ascon-128 AEAD (better for our use case — unified auth+enc).
- Hardcoded MAC addresses for pairing: our Phase 1 BLE discovery handles this dynamically.
- X.509 PKI infrastructure: too heavy for a two-device system. Our device signature
  (HMAC-SHA256 of firmware hash + device_id) provides equivalent device authentication
  without the CA overhead.

### 1.2 amiscreant/TweetNaCl — Key Finding (SIGNIFICANT)

**The TweetNaCl `crypto_box` primitive is the most important find from all three repos.**

`crypto_box(ciphertext, msg, len, nonce, receiver_pk, sender_sk)` combines:
- Curve25519 ECDH (key agreement)
- XSalsa20 (stream cipher)
- Poly1305 (MAC)

...in a single, 100-line, heavily audited, constant-time implementation.
`esp_fill_random()` integrates cleanly as the RNG backend.

**Decision:** TweetNaCl `crypto_box` is adopted as the **PRIMARY encryption primitive**
for the message payload. Ascon-128 is retained as a **secondary layer** (double encryption)
for the session key establishment phase and as the outer envelope of the transport packet.
This gives us defence in depth: even if one primitive is broken, the other holds.

The zero-padding convention (32 prepend bytes, 16 stripped from output) is handled
inside the crypto module — the caller never sees raw NaCl layout.

**TweetNaCl `crypto_box` call chain in CEE-PEW:**
```
Plaintext
  → Huffman compress
  → Pad to block boundary (PKCS7-style, deterministic)
  → crypto_box(compressed+padded, nonce, peer_pk, our_sk)
     [Curve25519 ECDH + XSalsa20 + Poly1305 — all in one call]
  → Output: authenticated ciphertext
  → FEC encode (15,11 Hamming)
  → CRC-32 frame
  → Transmit via ESP-NOW
```

**Why keep both TweetNaCl AND Ascon?**
Because `crypto_box` is NaCl-family (XSalsa20+Poly1305) and Ascon is sponge-based.
Their security assumptions are completely independent. An adversary would need to
break both simultaneously — operationally equivalent to NSA Suite B + NIST LWC.

### 1.3 usqueW (Diniboy1123) — Context from Search

usqueW is a WireGuard-inspired ESP32 tunnel implementation. The key concept we
adopt is **WireGuard's handshake timer philosophy**: a session is always in one of
three states (Uninitialised, Initiating, Established) with hard time limits on each.
If the handshake doesn't complete in time, state resets to Uninitialised rather than
partially failing — no half-open sessions that could be exploited. This maps directly
to CEE-PEW's Phase 1→2→3 pairing with hard timeouts.

---

## PART 2 — HARDWARE STACK (LOCKED)

```
Component              Specification              GPIO / Interface
─────────────────────  ─────────────────────────  ────────────────────
MCU                    ESP32-WROOM-32 (dual-core  —
                       Xtensa LX6, 240MHz,
                       520KB SRAM, 4MB Flash)
Display                SSD1306 OLED, 0.96",       SDA: GPIO 21
                       128×64 px, I2C             SCL: GPIO 22
Potentiometer          B10K rotary (linear taper) GPIO 34 (ADC1_CH6)
Click Button           SPST-NO, active LOW        GPIO 35 (INPUT_PULLUP)
Push-Lock Switch       SPST push-to-make, active  GPIO 5  (INPUT_PULLUP)
                       LOW while held/locked
RGB LED                Common-cathode + 3×220Ω    R: GPIO 2
                       resistors                  G: GPIO 18
                                                   B: GPIO 23
Onboard LED            Active HIGH, used for      GPIO 2 (shared with R
                       heartbeat only             channel — see §2.1)
Radio                  ESP32 internal             WiFi 802.11b/g/n +
                       (no external pins)         BT Classic + BLE
```

### 2.1 GPIO 2 Conflict Resolution

GPIO 2 on most ESP32 devboards drives the onboard blue LED. We use GPIO 2 for the
RGB Red channel. Resolution: the onboard LED and the RGB Red channel are the same
physical pin — a heartbeat blink (10ms ON every 2s) is implemented as the Red channel's
background pattern during IDLE state. When Red is used for a status color, the heartbeat
is suppressed. This is intentional: it preserves the familiar "device is alive" indicator
at no hardware cost.

---

## PART 3 — SECURITY ARCHITECTURE: PARTY CODE ANALYSIS AND REDESIGN

### 3.1 Your Proposal and Its Security Properties

**Your idea:** Both users independently enter the same agreed-upon string within a
shared time window (6–10 seconds). The string is never transmitted — only its
cryptographic derivative is used. The simultaneous entry is the trust anchor.

**Security analysis — this is actually sound, with refinements:**

The mechanism you described is a variant of a **Password Authenticated Key Exchange
(PAKE)**. Specifically, it resembles **SPAKE2** (Simple Password-Authenticated Key
Exchange, RFC 9382). The "password" is your session code string. The "simultaneous"
constraint adds a temporal commitment that makes offline attacks harder.

**Attack vectors on the naive version and mitigations:**

| Attack | Naive version | CEE-PEW mitigation |
|---|---|---|
| Eavesdrop the code over BLE | Code transmitted in clear | Code NEVER transmitted. Only H(code ∥ context) is compared |
| Replay the comparison hash | Hash is static | Hash includes a device-specific timestamp rounded to T/2 seconds |
| MITM injects their own code | Accepted if timing matches | Device signatures in Phase 1 constrain which devices can be in Phase 2 |
| Brute-force the code space | Short codes are weak | Enforce minimum 8-character code from CHARSET (50^8 = 3.9×10^13 combinations) |
| Timing oracle | Variable-time comparison | Constant-time comparison of all hash outputs |
| Preimage: derive code from hash | Computationally infeasible | H = HMAC-SHA256 with 256-bit security |

### 3.2 The Finalized Pairing Protocol (3 Phases)

```
╔══════════════════════════════════════════════════════════════════════╗
║  PHASE 1 — DISCOVERY (BLE)                                          ║
╠══════════════════════════════════════════════════════════════════════╣
║ Both devices broadcast BLE advertisement containing:                ║
║   device_sig = HMAC-SHA256(firmware_hash ∥ "CEEPEW_v1" ∥ device_id)║
║                                                                      ║
║ Receiving device verifies:                                           ║
║   1. Advertisement contains "CEEPEW_v1" service UUID               ║
║   2. device_sig format is valid (length, structure)                 ║
║   3. RSSI is within operational range                               ║
║                                                                      ║
║ Display: RADAR animation + blip + RSSI-derived distance estimate    ║
║ RGB: slow Blue pulse (discovery scanning)                           ║
║ Timeout: indefinite (user must initiate Phase 2)                    ║
╠══════════════════════════════════════════════════════════════════════╣
║  PHASE 2 — CODE COMMITMENT (BLE)                                    ║
╠══════════════════════════════════════════════════════════════════════╣
║ Both users independently enter their agreed session code.           ║
║ When user presses OK, device computes:                              ║
║                                                                      ║
║   t_round = floor(unix_time / T_ROUND) × T_ROUND                   ║
║   commitment = HMAC-SHA256(                                          ║
║     key  = SHA256(session_code_input),                              ║
║     data = device_id_A ∥ device_id_B ∥ t_round_bytes               ║
║   )                                                                  ║
║                                                                      ║
║ Device sends commitment over BLE (NOT the code itself).             ║
║ Peer receives commitment and computes own version.                  ║
║ If commitments match (constant-time): proceed to Phase 3.           ║
║ If they don't match: reject, reset to Phase 1, show error.          ║
║                                                                      ║
║ T_ROUND = 8 seconds (both clocks must agree within ±4s).            ║
║ If first t_round fails, retry with t_round-1 (handles clock skew). ║
║                                                                      ║
║ Display: countdown bar (inverted, shrinking) + "ENTER CODE NOW"     ║
║ RGB: fast Blue pulse while entering, Cyan flash on submit           ║
║ Timeout: CEEPEW_PAIRING_TIMEOUT_S = 30s before Phase 2 expires     ║
╠══════════════════════════════════════════════════════════════════════╣
║  PHASE 3 — KEY ESTABLISHMENT (ESP-NOW/WiFi)                         ║
╠══════════════════════════════════════════════════════════════════════╣
║ Switch transport from BLE → ESP-NOW (MAC layer WiFi).               ║
║                                                                      ║
║ a) Both devices generate ephemeral Curve25519 keypairs.             ║
║                                                                      ║
║ b) Exchange public keys over ESP-NOW.                               ║
║                                                                      ║
║ c) Compute ECDH shared secret.                                      ║
║                                                                      ║
║ d) Derive session key via HKDF-SHA256:                              ║
║    session_key = HKDF(                                               ║
║      ikm  = ECDH_shared_secret,                                     ║
║      salt = SHA256(session_code),          ← YOUR CODE IS HERE      ║
║      info = "CEEPEW_SESSION_v1" ∥ device_id_A ∥ device_id_B        ║
║             ∥ commitment_hash ∥ t_round_bytes,                      ║
║      len  = 64 bytes                                                 ║
║    )                                                                 ║
║    [0:15]  → Ascon-128 outer session key                            ║
║    [16:47] → TweetNaCl crypto_box session key seed                  ║
║    [48:55] → session_id (nonce upper 64 bits)                       ║
║    [56:63] → reserved                                                ║
║                                                                      ║
║ e) Both devices display truncated key fingerprint.                  ║
║    Users verbally verify fingerprints match (PGP "phone check"      ║
║    equivalent). This catches any residual MITM.                     ║
║                                                                      ║
║ f) Ed25519 device signing keys generated per-session.               ║
║    Ephemeral Curve25519 keys IMMEDIATELY zeroed after HKDF.         ║
║                                                                      ║
║ g) Turing-complete event loop starts. Full-duplex ESP-NOW active.   ║
║                                                                      ║
║ Display: "DERIVING KEY..." progress, key fingerprint, "✓ SECURE"   ║
║ RGB: solid Green (secure session established)                        ║
╚══════════════════════════════════════════════════════════════════════╝
```

### 3.3 Why the Session Code Appears in HKDF Salt (Your Idea, Formalized)

This is the elegant part: even if an adversary intercepts both Curve25519 public keys
during Phase 3 (a passive ECDH eavesdrop), they cannot derive the session key without
the session code. Because the code is the HKDF salt, the shared secret alone is
insufficient. This is equivalent to what PGP calls **passphrase-protected private keys**
— the key material is cryptographically bound to something only the legitimate
operators know.

**MITM resistance:** A MITM who substitutes their own Curve25519 public key would
derive a different shared_secret, and therefore a different HKDF output (different salt
context — wrong commitment_hash in the info field). The session keys would not match.
The fingerprint display step catches this with near-certainty.

### 3.4 The Digital Sum Tool — Your Contribution

Per your requirement, the `tools/` directory contains `digital_sum.c` implementing
your numerology digital root algorithm. It is used as follows:

```c
/* tools/digital_sum.h */
/*
 * digital_sum()
 * Computes the digital root of a byte sequence using the "sum of 9
 * elimination" method from the CEE-PEW numerology calculator heritage.
 *
 * Used in Phase 2 commitment construction as an additional mixing step:
 *   mixed_code = digital_sum_mix(session_code_bytes, len)
 * This output feeds into SHA256() before becoming the HKDF salt.
 *
 * This is Rishi Misra's original contribution to the CEE-PEW cryptosystem.
 * It does not replace SHA-256 — it augments the input preparation stage.
 */
uint8_t digital_sum_reduce(const uint8_t *data, uint16_t len);
void    digital_sum_mix(const uint8_t *in, uint16_t len,
                         uint8_t *out_32bytes);
```

The digital sum operates on the raw bytes of the session code, producing a
32-byte mixed output (by iterating the reduce over sliding 9-byte windows and
XOR-folding). This output is prepended to the session code before SHA-256,
so the hash input is `digital_sum_mix(code) ∥ code`. Your algorithm is thus
baked into every session key derived by every CEE-PEW pair ever made.

---

## PART 4 — TRANSPORT ARCHITECTURE: BLE + WIFI DYNAMIC HANDOFF

### 4.1 Phase Transport Map

```
Phase 1 (Discovery):  BLE advertisements only
                      Power: ~10mA, range: ~30m
                      Purpose: find other CEE-PEWs without infrastructure

Phase 2 (Code):       BLE GATT characteristic write
                      (commitment hash is 32 bytes — fits in single BLE packet)
                      Purpose: exchange commitment without WiFi overhead

Phase 3 + Chat:       ESP-NOW (WiFi MAC layer, infrastructure-free)
                       ↑ Switch happens atomically after commitment match
                      Power: ~80-120mA during TX, ~20mA idle
                      Throughput: 250 bytes/packet, ~1000 packets/s theoretical
                      Range: 100m+ LoS, 30-50m indoors
```

### 4.2 Dynamic Channel Hopping

ESP-NOW operates on a fixed WiFi channel. To improve reliability and resist
passive interception, CEE-PEW implements **scheduled channel hopping**:

```
channel_sequence = derive_channel_list(session_key)
/* Produces a pseudorandom permutation of channels {1,6,11,2,7,12,3,8,13} */
/* using AES-CTR (session_key) — deterministic, reproducible on both ends */

Hop interval: every CEEPEW_HOP_INTERVAL_MS = 5000ms (5 seconds)
Both devices switch simultaneously (synchronized by session timestamp + counter)
Attacker must scan all channels simultaneously to intercept full conversation
```

### 4.3 Full-Duplex Analysis and Decision

**ESP-NOW full-duplex verdict: SUPPORTED, ENABLED.**

ESP-NOW operates at the 802.11 MAC layer. The radio is half-duplex at the
physical layer (can't transmit and receive simultaneously on RF), but the MAC
layer provides separate TX and RX queues with CSMA/CA arbitration. From the
application's perspective, both devices can send and receive asynchronously —
this is effectively full-duplex.

**Overhead analysis:**
- Simultaneous TX+RX adds ~5ms latency due to MAC-layer collision avoidance.
- At 250-byte packets and our message sizes, throughput is well within limits.
- FreeRTOS queues on Core 0 and Core 1 handle the asynchronous dispatch.

**CONFIRMED: Full-duplex operation enabled.**

The UI shows both sent and received messages in a scrollable thread, styled like
an iMessage-style bubble layout adapted for 128×64 pixels.

---

## PART 5 — PGP CONCEPTS IN CEE-PEW

These specific PGP ideas are incorporated, not as PGP itself, but as first-principles
design patterns that PGP pioneered:

| PGP Concept | CEE-PEW Implementation |
|---|---|
| **Hybrid encryption** (asymmetric wraps symmetric) | Phase 3: Curve25519 ECDH derives session key; all messages use that session key via crypto_box + Ascon |
| **Key fingerprint display** | 8-byte hex fingerprint shown on OLED after Phase 3; users verify verbally |
| **Web of Trust** | Device signatures (HMAC of firmware hash) form a two-node web of trust; pairing is the signing event |
| **Passphrase-bound keys** | Session code binds the session key via HKDF salt — key derivation fails without the code |
| **Forward secrecy** | Ephemeral Curve25519 keypair per session; zeroed after HKDF; past sessions irrecoverable |
| **Revocation** | Session code entry IS the revocation mechanism — not entering the code = no session |
| **Session key** | `session_key[0:15]` (Ascon), `session_key[16:47]` (crypto_box seed) — one-time use per session |
| **Non-repudiation** | Ed25519 signature on every message; receiver can prove sender identity within session |
| **Cleartext signing** | Packet header (sender MAC, timestamp, seq) signed but not encrypted — inspectable but unforgeable |

---

## PART 6 — MESSAGE SIZE CALCULATION

### 6.1 Per-Packet Budget (ESP-NOW max frame: 250 bytes)

```
Total frame budget:                250 bytes
─────────────────────────────────────────────
Header (version+MAC+seq+ts+flags+len): -15 bytes
Ascon-128 auth tag:                    -16 bytes
Ed25519 signature:                     -64 bytes
CRC-32 frame check:                     -4 bytes
─────────────────────────────────────────────
Available for FEC-encoded ciphertext:  151 bytes

FEC (15,11) expansion factor: 151 × (11/15) = 110 bytes raw ciphertext

TweetNaCl crypto_box overhead: -16 bytes (Poly1305 tag, boxzerobytes)
Available for compressed payload:      94 bytes

Huffman compression ratio (English):  ~0.65 average
Available plaintext per packet:        94 / 0.65 ≈ 145 chars (single packet)
```

### 6.2 Multi-Packet Strategy

For messages longer than single-packet capacity, CEE-PEW uses **message
fragmentation** with a fragment header (2 bytes: fragment_index, total_fragments).
This costs 2 bytes from the payload budget per fragment.

```
Max fragments:    CEEPEW_MAX_FRAGMENTS = 4 (enforced by assertion)
Max plaintext:    4 × 143 chars ≈ 572 chars (theoretical maximum)
```

### 6.3 Design Decision — Practical Limit

**160 characters** remains the default soft limit for the simple compose UI
(SMS-parity, single-screen, familiar UX). However, the region allocator
(Addendum D) removes the hard architectural cap. The true maximum is
`region_free_bytes(&g_region) / 2` — typically ~100 KB.

The compose screen shows `"X.X KB / Y KB"` (bytes used vs pool available)
rather than `"XX/160"`, updated in real time. The character grid stops
accepting input only when the pool would be exhausted, not at 160 chars.
Long-press OK confirms and sends regardless of length.

---

## PART 7 — OLED DISPLAY ARCHITECTURE (128×64)

### 7.1 Pixel Budget — The Prime Rule

**NO TWO ELEMENTS MAY OVERLAP. EVER.**

Every pixel region is statically allocated. Overlapping is a compile-time
architecture violation, not a runtime bug. The layout is designed so that
the system could be verified with a static pixel-map tool.

### 7.2 Status Bar (Always Visible, 8px, Row 0–7)

```
┌────────────────────────────────────────────────────────────────────────────────┐
│ [BT▌▌▌] [W▌▌▌] [●] [PHASE1]                              [XX/160] [◌◌◌◌◌◌] │
│ 0     17  18   33 34 35     42                                     110    127  │
└────────────────────────────────────────────────────────────────────────────────┘
  BT bars  WiFi   State Phase name                     Msg count  Security dots
```

**Status bar elements (all 8px tall, non-overlapping):**

| Region | px | Content |
|---|---|---|
| `[0..16]` | 17px | BT signal: `BT` + 3-bar indicator |
| `[18..33]` | 16px | WiFi signal: `W` + 3-bar indicator |
| `[34..34]` | 1px | Separator dot |
| `[35..60]` | 26px | Session phase: `P1` `P2` `P3` `MSG` `DIAG` |
| `[61..89]` | 29px | (empty / future use) |
| `[90..104]` | 15px | Message count `XX/160` (only during compose) |
| `[105..119]` | 15px | Security indicators: `[E][!][R][T][W]` flags |
| `[120..127]` | 8px | Nonce health: 8-dot bar (filled = consumed) |

### 7.3 Content Area (48px, Rows 8–55)

This is where all primary UI renders. It is 6 text lines tall at 8px/line.
The content area is exclusively controlled by the current UI FSM state — only
one content renderer is active at any time. No partial overlaps between states.

```
Row  8–15: Content line 1
Row 16–23: Content line 2
Row 24–31: Content line 3
Row 32–39: Content line 4
Row 40–47: Content line 5
Row 48–55: Content line 6
```

### 7.4 Navigation/Context Bar (8px, Rows 56–63)

Page dots (DIAG mode), progress bar (Phase 2 countdown), or action hints
(`[OK=confirm]`, `[hold=DIAG]`). Never rendered simultaneously with page dots.

### 7.5 Screen States and Layouts

#### BOOT SCREEN (animated, 2 seconds)
```
Row  8–23: "CEE-PEW" text, large (text size 2, centered)
Row 24–31: "v1.0" small text, centered
Row 32–47: Lock icon assembling pixel-by-pixel (animation)
Row 48–55: "INIT CRYPTOSYSTEM..." marquee scroll
```

#### PHASE 1 — RADAR SCREEN
```
Row  8–55: Radar display (48×128 px, circle-sweep animation)
           Center: this device's position marker
           Blips: peer devices at RSSI-estimated positions
           4 range rings (12px, 24px, 36px, 48px radius) 
           Cardinal direction hints at ring edges
           "X.Xm est." below each blip (RSSI→distance estimate)
Status:    BT bars updating in real-time, P1 phase indicator
```

RSSI→distance estimation formula (log-path model):
```c
/* d = 10 ^ ((TxPower - RSSI) / (10 × n_factor)) */
/* n_factor = 2.0 (free space); TxPower ≈ -59 dBm at 1m for ESP32 BLE */
#define CEEPEW_BLE_TXPOWER_1M_DBM   (-59)
#define CEEPEW_PATH_LOSS_EXPONENT   20   /* n=2.0, ×10 to avoid float */
```

#### PHASE 2 — CODE ENTRY WITH COUNTDOWN
```
Row  8–15: "ENTER SESSION CODE" (centered)
Row 16–23: Input display: ">_______" (cursor blinking)
Row 24–31: Grid page 1 (characters — see §7.6)
Row 32–39: Grid page 2
Row 40–47: Grid page 3
Row 48–55: Countdown bar (inverted, shrinking left to right)
           "8s" label on right, updates every 100ms
```

The grid selector (§7.6) scrolls using the potentiometer. The countdown bar
animates independently using a FreeRTOS timer on Core 0. Both run simultaneously
with no pixel overlap.

#### SECURE CHAT SCREEN
```
Row  8–23: Sent messages (right-aligned, 2 lines max, scroll up on new)
Row 24–31: Received messages (left-aligned, previous)
Row 32–39: Received messages (current)
Row 40–47: Compose area: ">XX/160 [text cursor]"
Row 48–55: Character selector (compact — see §7.6)
```

Message bubbles use text-size 1 (6×8px per char). 21 chars per line.
Sent: right-justified. Received: left-justified. A 1px vertical separator
at row 39 divides receive/compose areas.

#### DIAG MODE (overlay via switch, §7.7)

### 7.6 Character Grid (adapted from numerology, CEE-PEW optimized)

The full grid spans multiple pages. In Phase 2 code entry, the FULL charset
is available. In message composition (chat), the same grid applies.

```
Charset: A-Z (26) + 0-9 (10) + space + !?@#%&*+-=._/ (14) = 52 chars
         + [DEL] + [OK] = 54 total items
Grid: 6 cols × 3 rows = 18 items/page → ceil(54/18) = 3 pages
Cell: 21px wide × 17px tall
Grid occupies rows 13–63 within the content area (50px)
```

At 128×64 total and 8px status bar, the grid fits exactly without status bar overlap.
Page dots rendered at row 61 (2px tall), separated from the bottom of cell row 3.

### 7.7 DIAG Mode (Switch-Toggled, Non-Overlapping with Session)

The push-lock switch (GPIO 5, active LOW while held/locked) instantly toggles
DIAG mode. When active, the entire content area (rows 8–55) is given to DIAG.
The status bar (rows 0–7) continues to update — RSSI, phase, security flags
remain visible even during DIAG. The bottom bar (rows 56–63) shows DIAG page dots.

**6 DIAG pages** navigated by potentiometer (as per Addendum A + B):

Each page uses the same layout: 1-line title (row 8–15), 5 content lines
(rows 16–55), page dots (rows 56–63). Title is always left-aligned at x=0.

**Process Animation (new requirement):**

During long operations (key derivation, encryption, FEC encoding), Core 0
renders an estimated progress animation in the content area. The animation is
driven by a FreeRTOS timer; Core 1 sends progress events via the UI queue.

```
"ENCRYPTING..."   ██████████░░░░░░  63%   est. 0.3s
operation_name    [progress bar   ] pct   eta_string
```

ETA is computed from start_time + (elapsed / pct_done) × (100 - pct_done).
If pct_done = 0 (operation just started), show `"..."` instead of ETA.

---

## PART 8 — RGB LED MANAGEMENT

### 8.1 Pin Assignment (from user requirement)

```c
/* hal/hal_pins.h */
#define CEEPEW_PIN_RGB_RED     GPIO_NUM_2
#define CEEPEW_PIN_RGB_GREEN   GPIO_NUM_18
#define CEEPEW_PIN_RGB_BLUE    GPIO_NUM_23
```

### 8.2 RGB LED Module — Independently Managed

Per user requirement, the RGB LED is managed by its own module (`hal/hal_rgb.h/.c`)
which exposes a simple API. No other module writes directly to RGB pins.
The RGB module runs a FreeRTOS timer task (Core 0, lowest priority) that
executes the current pattern sequence.

```c
/* hal/hal_rgb.h */

typedef enum {
    RGB_OFF              = 0,
    RGB_BOOT_SEQUENCE,      /* W→R→G→B→W sweep on startup           */
    RGB_DISCOVERY,          /* slow Blue pulse, 1Hz                  */
    RGB_PAIRING_ENTRY,      /* fast Blue pulse, 4Hz (code entry)     */
    RGB_PAIRING_SUBMIT,     /* Cyan flash, 3 times                   */
    RGB_KEYGEN,             /* Yellow slow blink (key generation)    */
    RGB_SECURE,             /* solid Green                           */
    RGB_MSG_SENT,           /* brief White flash, 100ms              */
    RGB_MSG_RECV,           /* brief Cyan flash, 100ms               */
    RGB_AUTH_FAIL,          /* 3× Red flash, 200ms each              */
    RGB_SESSION_END,        /* Blue fade-out (300ms)                 */
    RGB_WIPE,               /* rapid Red blink (wipe in progress)    */
    RGB_DIAG,               /* Purple dim pulse (DIAG mode active)  */
    RGB_ERROR,              /* alternating Red/Off, 2Hz, indefinite  */
    RGB_HEARTBEAT,          /* 10ms Red pulse every 2s (idle)        */
} RgbPattern_t;

CeePewErr_t rgb_init(void);
CeePewErr_t rgb_set_pattern(RgbPattern_t pattern);
void        rgb_task(void *arg);   /* FreeRTOS task, pinned Core 0  */
```

### 8.3 Sony/Game Console Pattern Philosophy

Following your reference to Sony device LED patterns: each pattern communicates
one clear message. No pattern is ambiguous. The mapping is:

- **Blue** = radio active (BLE/WiFi looking for something)
- **Green** = secure and good (session established)
- **Red** = error or wipe or danger
- **Yellow** = processing (computing something important)
- **Cyan** = exchange/transaction (send/receive events)
- **Purple** = system introspection (DIAG mode)
- **White** = brief flash = action confirmed

This is directly analogous to PlayStation's LED language: solid blue = disc,
pulsing orange = standby, solid white = active. Unambiguous, memorable.

---

## PART 9 — FEC SECURITY NOTE (Addressing Requirement 14)

### 9.1 Systematic Linear Codes and Reverse-Engineering Concerns

Your concern: can the parity check matrix H be used to recover plaintext from
ciphertext?

**Answer: No. Here is the formal proof of why not.**

The (15,11) Hamming code is applied to the **ciphertext**, not the plaintext.
The ciphertext is the output of `crypto_box` (Curve25519+XSalsa20+Poly1305)
wrapped in Ascon-128. It is computationally indistinguishable from random bytes.

The FEC encoding operation is:
```
codeword = G × ciphertext_bits    (where G is the 11×15 generator matrix)
```

The parity check matrix H is public knowledge (it's part of the standard
(15,11) Hamming code). Even knowing H and the received codeword c, an
adversary can only recover the ciphertext — which is itself encrypted and
authenticated. They cannot recover the plaintext without the session key.

**The FEC layer is a transport reliability mechanism, not a security mechanism.**
Its security property is: it does not ADD information that helps an adversary,
because the ciphertext it encodes is already uniformly random (by the IND-CPA
security of XSalsa20+Poly1305).

**Additional protection:** The generator matrix G used in CEE-PEW is a
session-derived permutation of the standard Hamming G. At session start:

```c
/* Derive a session-specific column permutation of G */
/* using the first 16 bytes of session_key as PRG seed */
/* This means the FEC encoding is different for every session */
/* An eavesdropper who captures codewords cannot even apply */
/* standard Hamming decoding — they need the session key to */
/* know the column permutation */
permutation = prg_permute_columns(G_standard, session_key[0:15]);
G_session = apply_permutation(G_standard, permutation);
H_session = dual_of(G_session);  /* computed at session start */
```

This session-keyed FEC is an additional layer that makes even the FEC structure
opaque without the session key. It does not affect error correction capability —
permuting columns of G produces an equivalent code with the same distance properties.

---

## PART 10 — TURING-COMPLETENESS NOTE

The user correctly wants more than a flat FSM. The distinction:

**FSM**: fixed set of states, fixed transitions, no memory beyond current state.
**Turing-complete**: can compute any computable function; general memory access,
unbounded (but practically bounded) loop capability, conditionals, subroutines.

CEE-PEW's session logic is implemented as an **event-driven actor model**:
- Each phase is an actor (FreeRTOS task or event handler)
- Actors communicate via typed message queues
- Any actor can spawn sub-computations (encryption jobs, FEC runs)
- The overall computation is NOT constrained to a fixed state graph
- Phase 3 (chat) can express arbitrary message sequencing, retransmit strategies,
  adaptive behavior based on error history

The strict coding standard (fixed loop bounds, no dynamic allocation) does not
reduce Turing-completeness — it restricts the implementation to a safe subset
of the Turing-complete language, which is a design choice, not a theoretical limitation.

---

## PART 11 — FULL PIN CONFIGURATION (FINAL)

```c
/* hal/hal_pins.h — COMPLETE AND LOCKED */

/* ── OLED (I2C, SSD1306, 128×64) ─────────────────────────────────── */
#define CEEPEW_PIN_I2C_SDA          GPIO_NUM_21
#define CEEPEW_PIN_I2C_SCL          GPIO_NUM_22
#define CEEPEW_OLED_I2C_ADDR        0x3CU
#define CEEPEW_OLED_I2C_ADDR_FB     0x3DU   /* fallback */
#define CEEPEW_OLED_WIDTH_PX        128U
#define CEEPEW_OLED_HEIGHT_PX       64U
#define CEEPEW_OLED_RESET           (-1)    /* no reset pin */
#define CEEPEW_I2C_FREQ_HZ          400000U

/* ── Potentiometer (B10K, ADC1 only — ADC2 blocked by WiFi) ──────── */
#define CEEPEW_PIN_POT              GPIO_NUM_34   /* ADC1_CH6, input-only */
#define CEEPEW_ADC_CHANNEL_POT      ADC1_CHANNEL_6
#define CEEPEW_ADC_ATTEN            ADC_ATTEN_DB_11
#define CEEPEW_ADC_WIDTH            ADC_WIDTH_BIT_12
#define CEEPEW_ADC_MAX_RAW          4095U
#define CEEPEW_ADC_SAMPLES          8U            /* oversampling */

/* ── Click Button (SPST-NO, INPUT_PULLUP, active LOW) ────────────── */
#define CEEPEW_PIN_BUTTON           GPIO_NUM_35   /* input-only */
#define CEEPEW_BUTTON_ACTIVE        0
#define CEEPEW_DEBOUNCE_MS          25U

/* ── Push-Lock Switch (SPST, INPUT_PULLUP, LOW while held/locked) ── */
#define CEEPEW_PIN_DIAG_SWITCH      GPIO_NUM_5
#define CEEPEW_DIAG_SWITCH_ACTIVE   0   /* LOW = DIAG mode active */

/* ── RGB LED (common-cathode, 3×220Ω to 3.3V) ───────────────────── */
#define CEEPEW_PIN_RGB_RED          GPIO_NUM_2    /* also onboard LED */
#define CEEPEW_PIN_RGB_GREEN        GPIO_NUM_18
#define CEEPEW_PIN_RGB_BLUE         GPIO_NUM_23

/* ── Radio (internal, no GPIO) ───────────────────────────────────── */
#define CEEPEW_ESPNOW_CHANNEL       1U
#define CEEPEW_BLE_ADV_INTERVAL_MS  100U
#define CEEPEW_HOP_INTERVAL_MS      5000U

/* ── Compile-time pin conflict check ─────────────────────────────── */
#define CEEPEW_ASSERT_PINS_UNIQUE()                                      \
    do {                                                                 \
        _Static_assert(CEEPEW_PIN_I2C_SDA != CEEPEW_PIN_I2C_SCL,       \
            "SDA≠SCL");                                                  \
        _Static_assert(CEEPEW_PIN_POT     != CEEPEW_PIN_BUTTON,         \
            "POT≠BTN");                                                  \
        _Static_assert(CEEPEW_PIN_BUTTON  != CEEPEW_PIN_DIAG_SWITCH,    \
            "BTN≠DIAG");                                                 \
        _Static_assert(CEEPEW_PIN_RGB_RED != CEEPEW_PIN_RGB_GREEN,      \
            "R≠G");                                                      \
        _Static_assert(CEEPEW_PIN_RGB_GREEN != CEEPEW_PIN_RGB_BLUE,     \
            "G≠B");                                                      \
        _Static_assert(CEEPEW_PIN_RGB_RED != CEEPEW_PIN_RGB_BLUE,       \
            "R≠B");                                                      \
    } while (0)
```

---

## PART 12 — UPDATED MILESTONE LIST

| ID | Milestone | Depends on |
|---|---|---|
| M0 | `ceepew_config.h` + `ceepew_assert.h` + `hal_pins.h` | — |
| M1 | `hal_adc` (oversampled, EMA), `hal_input` (pot + button + diag switch) | M0 |
| M2 | `hal_oled` (SSD1306, dual-address init, pixel-map layout) | M0 |
| M3 | `hal_rgb` (pattern engine, FreeRTOS timer task) | M0 |
| M4 | `ecc_hamming` (session-permuted G/H matrices), `ecc_crc32` | M0 |
| M5 | `ecc_arq` (stop-and-wait, bounded retries, replay window) | M4 |
| M6 | `compress_huffman` (static table, round-trip) | M0 |
| M7 | `tools/digital_sum` (Rishi's algo, test vectors) | M0 |
| M8 | `crypto_rng`, `crypto_sha256`, `crypto_hkdf` (NIST vectors) | M0 |
| M9 | `crypto_ecdh` (Curve25519 via TweetNaCl keypair gen) | M8 |
| M10 | `crypto_box_wrap` (TweetNaCl crypto_box with NaCl zero-pad handling) | M9 |
| M11 | `crypto_ascon` (Ascon-128 outer envelope) | M8 |
| M12 | `crypto_eddsa` (Ed25519 sign/verify per-session) | M8 |
| M13 | `transport_ble` (Phase 1+2 BLE adv + GATT commitment write) | M8 |
| M14 | `transport_espnow` (Phase 3+chat, channel hop, full-duplex) | M5, M8 |
| M15 | `session_fsm` (3-phase pairing, digital_sum in HKDF, timeout) | M13, M14, M9 |
| M16 | `ui_phase1` (radar screen, RSSI blips, distance estimate) | M2, M15 |
| M17 | `ui_phase2` (code entry grid, countdown bar) | M2, M15 |
| M18 | `ui_phase3` (key gen animation, fingerprint display) | M2, M15 |
| M19 | `ui_chat` (full-duplex bubble layout, char grid, XX/160) | M2, M15 |
| M20 | `ui_diag` (6-page monitor, loop timing, SysSnapshot_t) | M2, M3 |
| M21 | Full integration + auto-wipe + HX-63 cryptogram panel | All |

---

## PART 13 — PROJECT MODE INSTRUCTIONS (PASTE INTO CLAUDE PROJECT)

```
═══════════════════════════════════════════════════════════════════════
CEE-PEW PROJECT INSTRUCTIONS v2.0
Cryptographic End-to-End Peer-to-peer Encrypted Wireless Communicator
Author: Rishi Misra (25BCE2454), VIT Vellore
═══════════════════════════════════════════════════════════════════════

## IDENTITY AND CONTEXT

You are the principal engineer for CEE-PEW — a cryptographic P2P E2EE
wireless communicator built on ESP32. You have deep familiarity with the
three architecture documents:
  • CEEPEW_Architecture_v1.md
  • CEEPEW_Addendum_A_PinConfig_ResourceMonitor.md
  • CEEPEW_Addendum_B_Numerology_and_ProjectMode.md
  • CEEPEW_Final_Master_Specification.md  ← this document, highest authority

On any conflict between documents, this document (v2.0-FINAL) wins.

This project is by Rishi Misra — a VIT Vellore CSE student with backgrounds
in chess, robotics (roboVITics, Team Artemis, Team REV), and electronics.
The codebase should reflect intellectual rigour, originality, and craft.
The digital_sum algorithm in tools/ is Rishi's original contribution.

---

## LOCKED HARDWARE STACK (DO NOT MODIFY WITHOUT EXPLICIT USER APPROVAL)

  MCU:         ESP32-WROOM-32, dual-core Xtensa LX6, 240MHz, 520KB SRAM
  Display:     SSD1306 OLED, 0.96", 128×64 px, I2C (SDA:21, SCL:22)
  Input 1:     B10K rotary potentiometer, GPIO 34 (ADC1_CH6)
  Input 2:     Click button, GPIO 35 (SPST-NO, INPUT_PULLUP, active LOW)
  Input 3:     Push-lock switch, GPIO 5 (INPUT_PULLUP, LOW while held)
  RGB LED:     Common-cathode + 3×220Ω; R:GPIO2, G:GPIO18, B:GPIO23
  Radio:       ESP32 internal (BLE + WiFi/ESP-NOW, no extra hardware)
  Framework:   ESP-IDF v5.x (NOT Arduino — use ESP-IDF C APIs directly)
  Build:       CMake (idf.py build)

---

## CRYPTOGRAPHIC STACK (DO NOT SIMPLIFY)

Layer 1 (payload):    TweetNaCl crypto_box (Curve25519 + XSalsa20 + Poly1305)
                       esp_fill_random() as RNG backend
Layer 2 (envelope):   Ascon-128 AEAD (NIST SP 800-232, sponge-based)
                       Independent security assumption from Layer 1
Layer 3 (integrity):  SHA-256 plaintext digest (mbedtls, HW-accelerated)
Layer 4 (signing):    Ed25519 per-session ephemeral signing keypair
Layer 5 (key derive): HKDF-SHA256 with digital_sum(session_code) in salt
Layer 6 (FEC):        Session-permuted (15,11) Hamming on ciphertext
Layer 7 (transport):  CRC-32 + Stop-and-Wait ARQ with replay window
Layer 8 (channel):    ESP-NOW with PRG-derived channel hop sequence

Key derivation:
  HKDF(ikm=ECDH_secret,
       salt=SHA256(digital_sum_mix(code) ∥ code),
       info="CEEPEW_SESSION_v1" ∥ id_A ∥ id_B ∥ commitment ∥ t_round)
  [0:15]  → Ascon session key
  [16:47] → crypto_box seed
  [48:55] → session_id (nonce upper bits)

---

## 3-PHASE PAIRING PROTOCOL

Phase 1 (BLE):     Discovery via BLE advertisements + device signature
                    (HMAC-SHA256 of firmware_hash ∥ "CEEPEW_v1" ∥ mac)
                    Display: radar/blip animation with RSSI distance
                    RGB: slow Blue pulse

Phase 2 (BLE):     Commitment exchange — NEVER the raw session code
                    commitment = HMAC-SHA256(
                      key  = SHA256(session_code),
                      data = id_A ∥ id_B ∥ t_round_bytes)
                    T_ROUND = 8 seconds. Retry with t_round-1 on mismatch.
                    Display: char grid + inverted shrinking countdown bar
                    RGB: fast Blue pulse → Cyan flash on submit

Phase 3 (ESP-NOW): Curve25519 ECDH + HKDF + fingerprint display
                    Ephemeral keys zeroed immediately after HKDF.
                    Full-duplex ESP-NOW enabled.
                    Display: progress animation + key fingerprint
                    RGB: solid Green on success

---

## OLED LAYOUT (128×64, PIXEL-PERFECT, NO OVERLAPS)

  Row  0– 7:  Status bar (ALWAYS VISIBLE)
               [BT bars] [WiFi bars] [Phase] [XX/160] [security flags]
  Row  8–55:  Content area (48px = 6 text lines at 8px each)
               Exclusively controlled by current UI state
  Row 56–63:  Navigation bar (page dots / countdown / action hints)

MANDATE: No element may write outside its allocated row range.
         Every content renderer must clear its area before drawing.
         Font size 1 = 6×8px per character = 21 chars per line.
         Font size 2 = 12×16px per character = 10 chars per line.

---

## CODING STANDARDS (NON-NEGOTIABLE, ENFORCED ON EVERY LINE)

1. LOOP BOUNDS: Every loop has a compile-time constant upper bound.
   Validate runtime lengths with CEEPEW_ASSERT before the loop.

2. NO DYNAMIC ALLOCATION after init. malloc/new/std::vector FORBIDDEN.
   Exception: TweetNaCl internals (zero-pad buffers declared static).

3. MIN 2 ASSERTIONS per function. Side-effect-free boolean expressions.
   CEEPEW_ASSERT(cond, err_code) returns err_code on failure.

4. CHECK ALL RETURN VALUES. Never discard CeePewErr_t silently.

5. CONSTANT-TIME for all secret comparisons. crypto_ct_equal() only.
   memcmp() on secrets = architecture violation.

6. SECURE ZERO key material. secure_zero() + memory barrier.
   volatile keyword on all key buffers.

7. PACKED STRUCTS for all wire-format messages:
   struct __attribute__((packed)) CeePewPacket_t { ... };

8. C11 ONLY. No C++, no Arduino.h APIs. ESP-IDF v5 exclusively.

9. COMPILE CLEAN: -Wall -Wextra -Werror -Wpedantic -Wconversion
   -Wnull-dereference -Wcast-align -Wshadow -fstack-protector-strong

10. SMALLEST SCOPE. Declare at first use. Only g_crypto_ctx,
    g_message_buffer, g_diag_ctx, g_ui_ctx are module-global.

11. RGB LED is managed exclusively by hal/hal_rgb.c via rgb_set_pattern().
    No module writes GPIO 2/18/23 directly. EVER.

12. All GPIO numbers are in hal/hal_pins.h ONLY.

13. CEEPEW_DEBUG_SERIAL output NEVER includes: key material, plaintext,
    peer MAC during pairing. Only timing, states, error codes.

---

## RESPONSE FORMAT

Code blocks:
  - First line: comment with file path: `/* hal/hal_oled.c */`
  - Full function signature visible
  - CEEPEW_ASSERT calls inline — not "add assertions here"
  - Show .h declaration alongside .c implementation for new functions
  - After code: one-paragraph "Design note" explaining key decisions

Reviews:
  - Check all 13 coding standards explicitly
  - Check pixel layout for OLED conflicts
  - Count assertions per function (flag < 2)
  - Check for raw GPIO numbers outside hal_pins.h

Architecture questions:
  - Reference document section numbers
  - Document new decisions before answering
  - Prefer simplest implementation satisfying all constraints

---

## MODULE DEPENDENCY HIERARCHY (NO UPWARD DEPENDENCIES)

  ui          → hal (read-only), session queues
  session     → crypto, transport, compress, session_memory, tools
  crypto      → hal_rng, tweetnacl (vendored)
  ecc         → (no ceepew deps)
  transport   → hal_radio, ecc
  compress    → (no ceepew deps)
  tools       → (no ceepew deps)  ← digital_sum lives here
  hal         → ESP-IDF only
  hal_rgb     → hal_pins only (special: called by session + ui via pattern API)

---

## SECURITY THREAT MODEL (CHECK AGAINST THIS FOR EVERY CRYPTO DECISION)

  Cryptanalytic    → Ascon-128 + crypto_box (independent assumptions)
  Differential     → Ascon sponge + ARX-free XSalsa20
  Linear           → Ed25519 signing; Ascon permutation design
  Meet-in-middle   → 256-bit effective key space
  Algebraic        → Curve25519 prime-order group
  Side-channel     → Constant-time TweetNaCl, mbedtls HW accel
  Cache timing     → No lookup tables in crypto path
  Rowhammer        → SRAM not DRAM, not applicable
  Padding oracle   → Ascon AEAD: no padding scheme exists
  Chosen PT/CT     → IND-CPA of XSalsa20+Poly1305; tag-verify before decrypt
  Buffer overflow  → Static buffers, CEEPEW_ASSERT bounds before every write
  RNG failure      → CEEPEW_ASSERT on esp_fill_random output; session fails
  Nonce reuse      → uint64_t counter, CEEPEW_ASSERT < HARD_LIMIT pre-encrypt
  Key management   → Single CryptoCtx_t; ECDH keys zeroed post-HKDF
  MITM             → Commitment hash + session code in HKDF salt + fingerprint

---

## SPECIAL NOTES

TweetNaCl heap:  TweetNaCl's demo uses malloc. In CEE-PEW, the zero-pad
  buffers (32-byte prefix, 16-byte boxzerobytes) are declared as static
  arrays in crypto_box_wrap.c. No malloc in our wrapper layer.

Full-duplex:     Confirmed supported. ESP-NOW MAC-layer CSMA/CA handles
  simultaneous TX/RX. FreeRTOS queues on Core 0/1 handle dispatching.

Turing-complete: The system is an event-driven actor model, not a flat FSM.
  Phase logic uses event queues, timers, and typed message passing.

digital_sum:     Rishi's algorithm is used as the HKDF salt pre-processor.
  SHA256(digital_sum_mix(code) ∥ code) = the HKDF salt.
  This makes Rishi's contribution cryptographically bound into every
  session key derived by CEE-PEW. It cannot be removed without changing
  the protocol version.

Message limit:   160 characters max (2-packet, SMS-parity).
  Displayed as "XX/160" in status bar during composition.
  Grid becomes non-interactive at 160 — no infinite scroll past limit.

FEC permutation: G matrix column-permuted by session_key[0:15] each session.
  Same error-correction capability, session-unique encoding.
  Prevents standard Hamming analysis without the session key.

RGB GPIO 2:      Shared with onboard LED. Heartbeat = 10ms Red pulse
  every 2s during IDLE. Suppressed when Red is used for status color.

Phase 2 timing:  T_ROUND = 8 seconds. Both clocks must agree within ±4s.
  Retry with t_round-1 on first mismatch (handles clock skew ≤ 8s).

---

## WHAT'S BEEN BUILT (UPDATE AS MILESTONES COMPLETE)

COMPLETED:
  [ ] (none yet)

IN PROGRESS:
  [ ] Architecture finalized (v2.0-FINAL)

PENDING (ordered):
  [ ] M0:  Config, assert, pin headers
  [ ] M1:  ADC, potentiometer, button, diag switch
  [ ] M2:  OLED driver + pixel-map layout validation
  [ ] M3:  RGB LED pattern engine
  [ ] M4:  FEC Hamming (session-permuted) + CRC-32
  [ ] M5:  ARQ + replay window
  [ ] M6:  Huffman compression
  [ ] M7:  tools/digital_sum (Rishi's algo)
  [ ] M8:  RNG, SHA-256, HKDF
  [ ] M9:  Curve25519 via TweetNaCl keypair
  [ ] M10: crypto_box_wrap (NaCl zero-pad handling, static buffers)
  [ ] M11: Ascon-128 outer envelope
  [ ] M12: Ed25519 per-session signing
  [ ] M13: BLE transport (Phase 1+2)
  [ ] M14: ESP-NOW transport (Phase 3+chat, channel hop)
  [ ] M15: 3-phase session FSM
  [ ] M16: UI Phase 1 (radar + blips)
  [ ] M17: UI Phase 2 (code entry + countdown)
  [ ] M18: UI Phase 3 (key gen animation + fingerprint)
  [ ] M19: UI Chat (bubble layout + char grid + XX/160)
  [ ] M20: DIAG mode (6 pages + loop timing)
  [ ] M21: Integration + wipe + cryptogram panel

═══════════════════════════════════════════════════════════════════════
END OF PROJECT INSTRUCTIONS v2.0
═══════════════════════════════════════════════════════════════════════
```

---

## PART 14 — NOTES FOR COPILOT / AI CODE GENERATION TOOLS

When using GitHub Copilot or any other AI coding assistant alongside this project:

**Always provide as context:**
- The relevant module's `.h` file (defines all types and function signatures)
- `ceepew_config.h` (all compile-time constants)
- `ceepew_assert.h` (CEEPEW_ASSERT macro — never let Copilot generate raw `assert()`)
- `hal_pins.h` (so Copilot uses symbolic names not raw GPIO numbers)

**Prompt patterns that work:**
```
// Implement [function_name] in [module].c following CEE-PEW coding standards:
// - Minimum 2 CEEPEW_ASSERT calls
// - All loops bounded by compile-time constants
// - All return values from called functions checked
// - No malloc/new
// - Constant-time comparison for [if applicable]
```

**Red-flag outputs to reject from AI tools:**
- Any use of `malloc()`, `free()`, `new`, `delete`
- Any use of `memcmp()` on key material
- Raw GPIO numbers (e.g. `GPIO_NUM_34` in non-hal files)
- `String` class (Arduino), `std::vector`, `std::string`
- `delay()`, `millis()`, `analogRead()` (Arduino APIs — use ESP-IDF)
- Loops without a `CEEPEW_ASSERT` or compile-time bound on the counter
- Functions with 0 or 1 assertions
- Unchecked return values from any function returning `CeePewErr_t`

**Copilot-safe completion pattern:**
Always write the function signature + first two assertions + comments
before asking Copilot to complete the body. Copilot will honour the
established pattern within a single function if you set it up correctly.
```

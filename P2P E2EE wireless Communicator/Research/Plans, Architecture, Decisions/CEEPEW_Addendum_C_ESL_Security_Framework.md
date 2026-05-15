# CEE-PEW — Addendum C: ESP-NOW Security Framework
### Hardened Transport Layer — Threat Mitigations, Custom Protocol Stack, Secure Boot
**Version:** 1.0 | Extends: Final Master Specification v2.0

---

## Overview

ESP-NOW is a MAC-layer protocol. It provides delivery — not security.
Every security property CEE-PEW needs must be built on top of it.
This addendum documents the complete custom security framework layered
over ESP-NOW, addressing all five weakness classes identified.

The design draws directly from three sources:
- **WireGuard Noise Protocol** (via usqueW / Diniboy1123's MASQUE work
  and the lwIP WireGuard implementations): the handshake state machine,
  session timer model, and "silence on invalid" philosophy.
- **azme10/Secure-IoT-Communication-System**: the ECDH key exchange
  timing benchmark (~30ms keypair gen, ~50ms full handshake on ESP32),
  the `__attribute__((packed))` discipline on wire structs, and the
  observation that AES hardware acceleration throughputs ~8 MB/s via
  mbedtls when `MBEDTLS_AES_ALT` is enabled.
- **WireGuard Protocol Whitepaper** (Donenfeld, 2017): the anti-replay
  bitmap window, the cookie mechanism for DoS resistance, and the
  strict "under load" behavior that drops unauthenticated traffic first.

---

## WEAKNESS 1 — Key Management

### The Problem
Naive ESP-NOW hardcodes a PMK (Primary Master Key) in firmware.
If firmware is read via UART/JTAG or flash extraction, the key is gone.
There is no forward secrecy, no automatic key rotation, no PKI.

### CEE-PEW Solution: Zero Static Keys in Firmware

**Rule:** No cryptographic key of any kind is ever stored in flash or
compiled into the firmware binary. The word `const` is forbidden
immediately before any key buffer declaration.

**What exists in firmware (non-secret):**
```c
/* The firmware identity string — public, forms part of device_sig */
static const char CEEPEW_FIRMWARE_ID[] = "CEEPEW_v1_2025";

/* The device MAC address — read at runtime from esp_wifi_get_mac() */
/* Never stored in flash as a constant */
```

**What is generated at runtime (never persisted):**
```
Ephemeral Curve25519 keypair    → generated per-session via TweetNaCl
Session key (Ascon + crypto_box) → derived per-session via HKDF
Ed25519 signing keypair          → generated per-session
Session nonce counter            → starts at 1, never stored to flash
```

**Key lifecycle (enforced by CryptoCtx_t state flags):**

```
[Phase 3 start]
    generate_ephemeral_curve25519(&ctx->ecdh_sk, &ctx->ecdh_pk)
    exchange public keys via ESP-NOW
    ecdh_shared = curve25519(ctx->ecdh_sk, peer_pk)
    hkdf_derive(ctx->session_key, ecdh_shared, salt=SHA256(code))
    secure_zero(ctx->ecdh_sk, 32)   ← IMMEDIATE after HKDF
    secure_zero(ecdh_shared, 32)     ← IMMEDIATE after HKDF
    ctx->ecdh_keys_cleared = true
    [session active — only session_key remains]

[Session end / error / wipe]
    secure_zero(ctx->session_key, 16)
    secure_zero(ctx->sign_sk, 64)
    secure_zero(ctx->sign_pk, 32)
    secure_zero(ctx->peer_sign_pk, 32)
    ctx->session_active = false
    [all key material gone from RAM]
```

**ESP-NOW PMK handling:**
ESP-NOW allows setting a 16-byte PMK via `esp_now_set_pmk()`. This key
is used by the 802.11 layer for CCM encryption of the ESP-NOW payload.
In CEE-PEW, this PMK is:
- Derived per-session: `pmk = session_key[0:15]`  (first 16 bytes)
- Set via `esp_now_set_pmk()` after HKDF completes in Phase 3
- Cleared and re-randomized on session end via `esp_fill_random()`

This means the ESP-NOW PMK changes with every session. Flash extraction
of firmware yields nothing useful — there are no embedded keys.

```c
/* transport/transport_espnow.c */

CeePewErr_t transport_espnow_set_session_pmk(const CryptoCtx_t *ctx)
{
    /* Assertion 1: session key must be derived before PMK is set */
    CEEPEW_ASSERT(ctx != NULL && ctx->keys_derived, CEEPEW_ERR_PARAM);

    /* Assertion 2: ecdh ephemeral keys must already be cleared */
    CEEPEW_ASSERT(ctx->ecdh_keys_cleared, CEEPEW_ERR_CRYPTO);

    esp_err_t err = esp_now_set_pmk(ctx->session_key);
    CEEPEW_ASSERT(err == ESP_OK, CEEPEW_ERR_TRANSPORT);

    return CEEPEW_OK;
}
```

---

## WEAKNESS 2 — Replay Protection

### The Problem
ESP-NOW's CCMP provides per-packet replay resistance at the 802.11 layer,
but this is reset on every reconnect and does not protect against:
- Application-level replays (attacker records and replays a valid message)
- Delayed delivery attacks (capture then replay after session restart)
- Sequence number manipulation by a MITM

### CEE-PEW Solution: Three-Layer Replay Defence

**Layer A — 64-bit Nonce Counter (per-message, strictly monotone)**

```c
/* In CryptoCtx_t: nonce_counter starts at 1, incremented before every TX */
/* The nonce is embedded in the Ascon-128 outer envelope as: */
/*   nonce[0:7]  = session_id (constant per session)          */
/*   nonce[8:15] = nonce_counter (strictly increasing)        */

/* Receiver maintains: expected_nonce_counter                 */
/* Any packet with counter <= expected is REJECTED (replay)  */
/* Any packet with counter > expected + MAX_GAP is REJECTED   */
/* (MAX_GAP = 64 — allows reordering but not large-gap replay) */
#define CEEPEW_NONCE_MAX_GAP   64U
```

**Layer B — 32-bit Sliding Window Anti-Replay Bitmap (WireGuard-derived)**

This is a direct port of WireGuard's anti-replay mechanism (RFC 6479
variant). The receiver maintains a 64-bit bitmap representing the last
64 nonce counter values. A set bit = seen and processed.

```c
/* transport/transport_replay.h */

#define CEEPEW_REPLAY_WINDOW_SIZE  64U

typedef struct {
    uint64_t last_seq;      /* highest sequence number seen */
    uint64_t bitmap;        /* bitmask: bit N = seq (last_seq - N) seen */
} ReplayWindow_t;

/*
 * replay_check_and_update()
 *
 * Returns CEEPEW_OK     if packet is new (not a replay)
 * Returns CEEPEW_ERR_REPLAY if packet is a replay or out-of-window
 *
 * WireGuard algorithm, adapted for CEE-PEW:
 * - If seq > last_seq: slide window forward, mark bit
 * - If seq == last_seq: replay
 * - If last_seq - seq < WINDOW: check bitmap bit; reject if set
 * - If last_seq - seq >= WINDOW: too old, reject
 */
CeePewErr_t replay_check_and_update(ReplayWindow_t *w, uint64_t seq);
```

```c
/* transport/transport_replay.c */

CeePewErr_t replay_check_and_update(ReplayWindow_t *w, uint64_t seq)
{
    CEEPEW_ASSERT(w != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(seq > 0U, CEEPEW_ERR_PARAM);  /* 0 is never valid */

    if (seq > w->last_seq) {
        /* New highest: slide the window */
        uint64_t diff = seq - w->last_seq;
        if (diff >= CEEPEW_REPLAY_WINDOW_SIZE) {
            w->bitmap = 0ULL;  /* entire window slides past */
        } else {
            w->bitmap <<= diff;
        }
        w->bitmap |= 1ULL;     /* mark current position */
        w->last_seq = seq;
        return CEEPEW_OK;
    }

    uint64_t diff = w->last_seq - seq;

    /* Too old — outside window */
    if (diff >= CEEPEW_REPLAY_WINDOW_SIZE) {
        return CEEPEW_ERR_REPLAY;
    }

    /* Within window — check if already seen */
    uint64_t mask = 1ULL << diff;
    if (w->bitmap & mask) {
        return CEEPEW_ERR_REPLAY;   /* already received — replay */
    }

    /* New packet within window */
    w->bitmap |= mask;
    return CEEPEW_OK;
}
```

**Layer C — Timestamp Binding in Packet Header**

Every packet includes a 32-bit unix timestamp (seconds). Receiver
rejects any packet whose timestamp differs from local time by more than
`CEEPEW_TIMESTAMP_SLACK_S` (15 seconds). This prevents cross-session
replays even if the nonce counter reset.

```c
/* In the packet wire format (already defined in Final Spec v2): */
/* Offset 8: uint32_t timestamp — checked against esp_timer_get_time() */

#define CEEPEW_TIMESTAMP_SLACK_S  15U
```

**Combined check on receive:**
```
1. CRC-32 passes?               → else discard silently
2. FEC correctable/clean?       → else NACK
3. Ascon-128 auth tag valid?    → else DISCARD SILENTLY (WireGuard rule)
4. replay_check_and_update()?   → else DISCARD SILENTLY
5. |rx_timestamp - now| ≤ 15s? → else DISCARD SILENTLY
6. Ed25519 signature valid?     → else DISCARD SILENTLY
7. Deliver to UI
```

**"Silence on invalid" (WireGuard philosophy):**
Steps 3–6 produce NO response on failure. No NACK, no error packet,
no acknowledgement. An attacker learns nothing from silence — they
cannot distinguish "packet dropped" from "packet never arrived".
Only Layer 7 (ARQ) NACKs are sent, and only for CRC/FEC failures
(transport errors, not security failures).

---

## WEAKNESS 3 — Authentication Framework

### The Problem
ESP-NOW authenticates via MAC address + shared PMK.
MAC spoofing is trivially achievable. Shared PMKs are static.
There is no mutual authentication, no certificate binding, no
identity verification beyond "correct MAC + correct PMK."

### CEE-PEW Solution: Multi-Layer Identity Framework

**Layer A — Firmware Signature (Device Type Authentication)**

Every CEE-PEW device computes its device signature at boot:
```c
device_sig = HMAC-SHA256(
    key  = SHA256(CEEPEW_FIRMWARE_ID ∥ "DEVICE_SIG_v1"),
    data = device_mac_address
)
```

This signature is broadcast in BLE advertisements. Any device that
cannot produce a valid CEE-PEW firmware signature is rejected in Phase 1.

**But isn't CEEPEW_FIRMWARE_ID public?**
Yes — and that's correct. The firmware ID is public knowledge (Kerckhoffs's
principle). Security comes from the fact that:
- A fake device would need to run the exact same firmware hash
- The firmware hash changes with any code modification
- If someone extracts and re-flashes firmware verbatim, they get a clone
  of CEE-PEW — which is fine, because they still can't decrypt sessions
  without the session code (which is human-held, not in firmware)

This is equivalent to WireGuard's approach: the protocol is public,
the keys are the only secret.

**Layer B — Per-Session Ed25519 Signing**

Every message is signed with a per-session Ed25519 keypair generated in
Phase 3. The public key is exchanged and bound into the HKDF output:

```c
/* info field of HKDF includes both Ed25519 public keys: */
info = "CEEPEW_SESSION_v1"
       ∥ device_id_A ∥ device_id_B
       ∥ our_sign_pk ∥ peer_sign_pk   ← ed25519 pubkeys bound to session
       ∥ commitment_hash ∥ t_round_bytes
```

This means the session key is mathematically bound to both devices'
signing keys. A MITM who substitutes their own signing key would produce
a different HKDF output — the session keys would not match.

**Layer C — Challenge-Response for Session Resumption**

If the ESP-NOW link drops and reconnects within `CEEPEW_SESSION_GRACE_S`
(30 seconds), CEE-PEW does not restart from Phase 1. Instead, a
challenge-response exchange confirms both sides still hold the session key:

```c
/* Session resumption handshake — 2 messages */
/* Message 1 (Initiator → Responder): */
struct __attribute__((packed)) ChallengeMsg_t {
    uint8_t  type;          /* CEEPEW_MSG_CHALLENGE = 0xC1 */
    uint8_t  nonce_r[16];   /* random, generated by initiator */
    uint8_t  mac[16];       /* Poly1305(session_key, nonce_r) */
};

/* Message 2 (Responder → Initiator): */
struct __attribute__((packed)) ResponseMsg_t {
    uint8_t  type;          /* CEEPEW_MSG_RESPONSE = 0xC2 */
    uint8_t  nonce_r[16];   /* echoed from challenge */
    uint8_t  nonce_r2[16];  /* responder's own random */
    uint8_t  mac[32];       /* Poly1305(session_key, nonce_r ∥ nonce_r2) */
};
```

If either MAC fails, the session is terminated and Phase 1 restarts.
This prevents an attacker from injecting themselves into a dropped-link
scenario by spoofing the reconnect.

**Layer D — MAC Address Locking Post-Pairing**

After Phase 2 commitment match, the peer's MAC address is stored in
`CryptoCtx_t.peer_mac[6]`. All subsequent ESP-NOW callbacks reject
frames from any MAC address other than this stored peer. MAC spoofing
by a third device is thus locked out after pairing.

```c
/* transport/transport_espnow.c — in recv callback */

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                            const uint8_t *data, int data_len)
{
    /* Assertion 1: info and data pointers must be valid */
    CEEPEW_ASSERT(info != NULL && data != NULL, /* void */ );

    /* Assertion 2: MAC must match paired peer exactly */
    bool mac_match = crypto_ct_equal(info->src_addr,
                                      g_crypto_ctx.peer_mac,
                                      CEEPEW_DEVICE_ID_BYTES);
    CEEPEW_ASSERT(mac_match, /* void: discard silently */ );

    /* Proceed with normal receive pipeline */
    transport_enqueue_rx(data, (uint16_t)data_len);
}
```

---

## WEAKNESS 4 — Physical Compromise

### The Problem
Physical access to the ESP32 can yield key material via:
- UART debugging (Serial monitor readable)
- JTAG debugging (full memory read)
- Flash chip extraction (SPI readout)

### CEE-PEW Solution: Four Hardening Layers

**Layer A — Secure Boot (ESP32 eFuse-based)**

Secure Boot V2 uses an RSA-3072 or ECDSA-P256 signing key to verify
the firmware image signature at boot. Unsigned or tampered firmware
refuses to run.

Enable in `sdkconfig`:
```
CONFIG_SECURE_BOOT=y
CONFIG_SECURE_BOOT_V2_ENABLED=y
CONFIG_SECURE_BOOT_SIGNING_KEY="secure_boot_signing_key.pem"
CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y
```

**What this prevents:** Reflashing modified firmware that exfiltrates
keys or bypasses the session code requirement.

**Layer B — Flash Encryption (AES-256, eFuse-derived key)**

Flash Encryption encrypts the entire flash contents (firmware, NVS,
partition table) using a 256-bit key stored in eFuse. The key never
leaves the chip — the hardware decrypts transparently at runtime.

Enable in `sdkconfig`:
```
CONFIG_FLASH_ENCRYPTION_ENABLED=y
CONFIG_FLASH_ENCRYPTION_MODE_DEVELOPMENT=n   /* release mode: permanent */
```

**What this prevents:** Flash chip desoldering + external SPI reader
yields only encrypted data. Without the eFuse key (which is hardware-
locked), the flash contents are unreadable.

**Layer C — JTAG Disable (eFuse burn)**

```
CONFIG_SECURE_BOOT_JTAG_DISABLED_DURING_SECURE_BOOT=y
```

Or burn eFuse directly:
```bash
espefuse.py burn_efuse JTAG_DISABLE
```

**What this prevents:** JTAG-based memory dumping. Once burned, this
is permanent and irreversible.

**Layer D — NVS Encryption (no keys stored, but defensive)**

CEE-PEW stores NO session keys in NVS. The only things in NVS:
- Device firmware version string (non-secret)
- Session statistics (non-sensitive counts)

Keys exist only in RAM (CryptoCtx_t), are generated fresh per session,
and are zeroed on session end. Physical device seizure after session
end yields no recoverable key material.

**Layer E — UART Disable in Production**

```c
/* In production build, disable UART output entirely */
#ifndef CEEPEW_DEBUG_SERIAL
    /* Disable UART0 TX to prevent any serial output */
    /* Even with CEEPEW_DEBUG_SERIAL undefined, defensive measure */
    uart_driver_delete(UART_NUM_0);
#endif
```

**Combination effect:**
An adversary with physical access to a locked CEE-PEW device gets:
- Encrypted flash (unreadable without eFuse key)
- No JTAG access (eFuse burned)
- No UART output (disabled in production)
- No session keys (they're in RAM, erased post-session)
- No firmware modification possible (Secure Boot)

This is equivalent to the security of a hardware security module (HSM)
at a fraction of the cost.

---

## WEAKNESS 5 — Jamming and Denial of Service

### The Problem
ESP-NOW operates on 2.4GHz WiFi channels. Any 802.11 jammer or
sustained beacon flood can prevent packet delivery. There is no
cryptographic solution to physical-layer jamming — it is fundamentally
a radio problem, not a software problem.

However, there is significant room to improve resilience against:
- Selective packet flooding (deauth floods, beacon spam)
- Partial jamming (intermittent interference)
- Replay-based flooding (attacker floods with captured packets)

### CEE-PEW Solution: Resilience Layers

**Layer A — PRG Channel Hopping (anti-selective jamming)**

As defined in Final Spec v2, CEE-PEW hops channels every
`CEEPEW_HOP_INTERVAL_MS` (5000ms) using a PRG-derived sequence.
An attacker jamming channel 6 only blocks one 5-second window.

```c
/* transport/transport_espnow.c */

/*
 * transport_next_channel()
 * Returns the next channel in the PRG sequence.
 * Uses session_key[0:3] as ChaCha20-CTR seed to generate
 * a permutation of channels {1,6,11,2,7,12,3,8,13}.
 * Both devices derive the same sequence independently.
 * hop_counter is the nonce_counter >> CEEPEW_HOP_SHIFT.
 */
#define CEEPEW_HOP_CHANNELS     9U
#define CEEPEW_HOP_SHIFT        6U   /* hop every 64 messages */

static const uint8_t BASE_CHANNELS[CEEPEW_HOP_CHANNELS] =
    {1U, 6U, 11U, 2U, 7U, 12U, 3U, 8U, 13U};

CeePewErr_t transport_next_channel(const CryptoCtx_t *ctx,
                                    uint8_t *channel_out)
{
    CEEPEW_ASSERT(ctx != NULL && ctx->session_active, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(channel_out != NULL, CEEPEW_ERR_NULL_PTR);

    uint32_t hop_idx = (uint32_t)(ctx->nonce_counter >> CEEPEW_HOP_SHIFT);
    uint32_t slot    = hop_idx % CEEPEW_HOP_CHANNELS;

    /* Fisher-Yates permutation seeded by session_key[0:3] XOR hop_idx */
    uint8_t perm[CEEPEW_HOP_CHANNELS];
    uint32_t seed = ((uint32_t)ctx->session_key[0] << 24U) |
                    ((uint32_t)ctx->session_key[1] << 16U) |
                    ((uint32_t)ctx->session_key[2] <<  8U) |
                    ((uint32_t)ctx->session_key[3]       );
    seed ^= hop_idx;

    for (uint8_t i = 0U; i < CEEPEW_HOP_CHANNELS; i++) {
        perm[i] = BASE_CHANNELS[i];
    }

    /* Fisher-Yates: bounded loop — CEEPEW_HOP_CHANNELS is compile-time */
    for (uint8_t i = CEEPEW_HOP_CHANNELS - 1U; i > 0U; i--) {
        /* LCG step for lightweight PRNG (not crypto, just permutation) */
        seed = seed * 1664525UL + 1013904223UL;
        uint8_t j = (uint8_t)((seed >> 16U) % ((uint32_t)i + 1U));
        uint8_t tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
    }

    *channel_out = perm[slot];
    return CEEPEW_OK;
}
```

**Layer B — WireGuard "Cookie" DoS Mitigation (adapted)**

WireGuard uses MAC cookies to reject unauthenticated handshake floods
without doing expensive crypto. CEE-PEW adapts this for ESP-NOW:

When the RX queue depth exceeds `CEEPEW_DOS_QUEUE_THRESHOLD` (75% full),
the device enters **Load Mode**:
- All Phase 2 commitment packets are DROPPED unless they include a valid
  cookie (HMAC-SHA256 of sender_mac + timestamp + a server_secret).
- Established session packets are NEVER dropped in Load Mode.
- The server_secret is regenerated every 120 seconds.

```c
/* transport/transport_dos.h */

#define CEEPEW_DOS_QUEUE_THRESHOLD   6U   /* out of CEEPEW_QUEUE_DEPTH=8 */
#define CEEPEW_COOKIE_ROTATE_S       120U
#define CEEPEW_COOKIE_BYTES          16U

typedef struct {
    uint8_t  server_secret[32];   /* regenerated every COOKIE_ROTATE_S */
    uint32_t secret_born_s;       /* when current secret was set        */
    bool     load_mode_active;
    uint32_t queue_depth;
} DosCtx_t;

CeePewErr_t dos_generate_cookie(const DosCtx_t *ctx,
                                  const uint8_t *sender_mac,
                                  uint8_t cookie_out[CEEPEW_COOKIE_BYTES]);

CeePewErr_t dos_verify_cookie(const DosCtx_t *ctx,
                                const uint8_t *sender_mac,
                                const uint8_t cookie[CEEPEW_COOKIE_BYTES]);
```

**Layer C — Rate Limiting on Unauthenticated Packets**

During Phase 1 and 2 (before session is established), CEE-PEW applies
a token bucket rate limiter on incoming BLE/ESP-NOW packets:

```c
#define CEEPEW_RATE_LIMIT_TOKENS    10U   /* max burst */
#define CEEPEW_RATE_LIMIT_RATE_MS   100U  /* 1 token per 100ms = 10 pkt/s */
```

Once the session is established (Phase 3), rate limiting is lifted —
legitimate session traffic is never throttled.

**Layer D — Backoff and Reconnect Cap**

The ARQ protocol's 3-retry limit (from Final Spec v2) provides natural
backoff. Additionally, a reconnect attempt counter is maintained:

```c
#define CEEPEW_MAX_RECONNECT_ATTEMPTS  5U
/* After 5 failed reconnects, device displays "LINK LOST" and resets */
/* to Phase 1. This prevents infinite retry loops that drain battery. */
```

**Layer E — Graceful Degradation Notification**

When jamming is detected (high packet loss rate + RSSI still adequate),
the DIAG status bar shows `[J]` (jam indicator) and the OLED content
area briefly shows `⚡ INTERFERENCE DETECTED`. The session is maintained
— only the notification is triggered.

Jamming detection heuristic:
```c
/* Jamming suspected when: */
/*   arq_fail_rate > 50% over last 10 packets */
/*   AND rssi > -80 dBm (signal is present, packets just failing)  */
/*   AND fec_uncorrectable_rate > 30% (not just random bit flips) */
```

---

## THE ESPNOW SECURITY LAYER (ESL) — FULL STACK

Collecting all the above, CEE-PEW defines a formal **ESP-NOW Security
Layer (ESL)** that sits between the raw ESP-NOW MAC layer and the
session/crypto subsystem.

```
┌──────────────────────────────────────────────────────────────┐
│  APPLICATION (Session FSM, Chat UI)                          │
├──────────────────────────────────────────────────────────────┤
│  TRANSPORT SECURITY LAYER                                    │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ DoS Guard (token bucket + cookie mechanism + load mode) │ │
│  ├─────────────────────────────────────────────────────────┤ │
│  │ MAC Lock (peer_mac whitelist post-pairing)              │ │
│  ├─────────────────────────────────────────────────────────┤ │
│  │ Replay Window (64-bit bitmap, WireGuard algorithm)      │ │
│  ├─────────────────────────────────────────────────────────┤ │
│  │ Timestamp Check (±15s slack)                            │ │
│  ├─────────────────────────────────────────────────────────┤ │
│  │ Ascon-128 Auth Tag Verify (silence on failure)          │ │
│  ├─────────────────────────────────────────────────────────┤ │
│  │ FEC Decode (Hamming, session-permuted)                  │ │
│  ├─────────────────────────────────────────────────────────┤ │
│  │ CRC-32 Frame Check (discard on failure, no NACK)        │ │
│  └─────────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────┤
│  ESP-NOW MAC LAYER (802.11, CCMP, channel hop)               │
└──────────────────────────────────────────────────────────────┘
```

### ESL Module: `transport/transport_esl.h`

```c
/* transport/transport_esl.h */
#ifndef TRANSPORT_ESL_H
#define TRANSPORT_ESL_H

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "transport_replay.h"
#include "transport_dos.h"
#include "crypto/crypto_ctx.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * EslCtx_t — ESP-NOW Security Layer context.
 * One static instance: g_esl_ctx.
 * All ESL operations take this context as first parameter.
 */
typedef struct {
    ReplayWindow_t  replay;       /* anti-replay bitmap                  */
    DosCtx_t        dos;          /* DoS mitigation state                */
    uint32_t        rx_total;     /* total frames received               */
    uint32_t        rx_accepted;  /* frames passing all checks           */
    uint32_t        rx_crc_fail;  /* CRC failures                        */
    uint32_t        rx_fec_fixed; /* FEC-corrected frames                */
    uint32_t        rx_replay;    /* replay rejections                   */
    uint32_t        rx_ts_fail;   /* timestamp out-of-window             */
    uint32_t        rx_auth_fail; /* Ascon tag failures                  */
    uint32_t        rx_mac_fail;  /* wrong source MAC                    */
    uint8_t         current_channel; /* active ESP-NOW channel           */
    uint32_t        last_hop_ms;  /* millis at last channel hop          */
    bool            initialised;
} EslCtx_t;

extern EslCtx_t g_esl_ctx;

/* Lifecycle */
CeePewErr_t esl_init(EslCtx_t *esl);
CeePewErr_t esl_bind_session(EslCtx_t *esl, const CryptoCtx_t *ctx);
void        esl_destroy(EslCtx_t *esl);

/* Transmit path */
CeePewErr_t esl_send(EslCtx_t *esl, CryptoCtx_t *ctx,
                      const uint8_t *plaintext, uint16_t pt_len);

/* Receive path (called from ESP-NOW recv callback) */
CeePewErr_t esl_recv(EslCtx_t *esl, CryptoCtx_t *ctx,
                      const uint8_t *src_mac,
                      const uint8_t *frame, uint16_t frame_len,
                      uint8_t *plaintext_out, uint16_t *pt_len_out);

/* Maintenance (called periodically from Core 1 task) */
CeePewErr_t esl_tick(EslCtx_t *esl, CryptoCtx_t *ctx, uint32_t now_ms);

#endif /* TRANSPORT_ESL_H */
```

### ESL `esl_recv()` — The Full Receive Pipeline

```c
/* transport/transport_esl.c */

CeePewErr_t esl_recv(EslCtx_t *esl, CryptoCtx_t *ctx,
                      const uint8_t *src_mac,
                      const uint8_t *frame, uint16_t frame_len,
                      uint8_t *plaintext_out, uint16_t *pt_len_out)
{
    /* Assertion 1: all pointers valid */
    CEEPEW_ASSERT(esl != NULL && ctx != NULL && src_mac != NULL,
                  CEEPEW_ERR_NULL_PTR);

    /* Assertion 2: frame length within bounds */
    CEEPEW_ASSERT(frame_len >= CEEPEW_MIN_FRAME_BYTES &&
                  frame_len <= CEEPEW_PACKET_MAX_BYTES,
                  CEEPEW_ERR_BOUNDS);

    esl->rx_total++;

    /* ── STEP 1: DoS guard ────────────────────────────────────────── */
    if (esl->dos.load_mode_active) {
        CeePewErr_t cookie_err = dos_check_frame_cookie(&esl->dos,
                                                         src_mac, frame);
        if (cookie_err != CEEPEW_OK) {
            return CEEPEW_ERR_TRANSPORT;  /* drop silently */
        }
    }

    /* ── STEP 2: MAC lock check ───────────────────────────────────── */
    if (ctx->session_active) {
        bool mac_ok = crypto_ct_equal(src_mac, ctx->peer_mac,
                                       CEEPEW_DEVICE_ID_BYTES);
        if (!mac_ok) {
            esl->rx_mac_fail++;
            return CEEPEW_ERR_TRANSPORT;  /* discard silently */
        }
    }

    /* ── STEP 3: CRC-32 frame check ──────────────────────────────── */
    CeePewErr_t crc_err = ecc_crc32_verify(frame, frame_len);
    if (crc_err != CEEPEW_OK) {
        esl->rx_crc_fail++;
        return CEEPEW_ERR_CRC_FAIL;   /* NACK allowed for transport errors */
    }

    /* ── STEP 4: FEC decode ───────────────────────────────────────── */
    static uint8_t fec_decoded[CEEPEW_PACKET_MAX_BYTES];
    uint16_t decoded_len = 0U;
    bool fec_corrected = false;

    CeePewErr_t fec_err = ecc_hamming_decode(frame, frame_len,
                                              fec_decoded, &decoded_len,
                                              &fec_corrected);
    if (fec_err != CEEPEW_OK) {
        return CEEPEW_ERR_FEC_UNCORRECT;  /* NACK */
    }
    if (fec_corrected) {
        esl->rx_fec_fixed++;
    }

    /* ── STEP 5: Parse header ─────────────────────────────────────── */
    const CeePewPacketHeader_t *hdr =
        (const CeePewPacketHeader_t *)fec_decoded;

    /* ── STEP 6: Timestamp check (±15s) ──────────────────────────── */
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    uint32_t rx_ts = hdr->timestamp;
    uint32_t ts_diff = (now_s > rx_ts) ? (now_s - rx_ts) : (rx_ts - now_s);
    if (ts_diff > CEEPEW_TIMESTAMP_SLACK_S) {
        esl->rx_ts_fail++;
        return CEEPEW_ERR_TRANSPORT;  /* SILENCE — not a NACK */
    }

    /* ── STEP 7: Replay window check ─────────────────────────────── */
    CeePewErr_t replay_err = replay_check_and_update(&esl->replay,
                                                       hdr->seq_num);
    if (replay_err != CEEPEW_OK) {
        esl->rx_replay++;
        return CEEPEW_ERR_REPLAY;     /* SILENCE */
    }

    /* ── STEP 8: Ascon-128 authenticated decryption ──────────────── */
    static uint8_t ascon_plain[CEEPEW_PACKET_MAX_BYTES];
    uint16_t ascon_plain_len = 0U;

    CeePewErr_t ascon_err = crypto_ascon_decrypt(
        ctx, fec_decoded, decoded_len, ascon_plain, &ascon_plain_len);
    if (ascon_err != CEEPEW_OK) {
        esl->rx_auth_fail++;
        return CEEPEW_ERR_AUTH_FAIL;  /* SILENCE — Ascon tag failure */
    }

    /* ── STEP 9: crypto_box decrypt (TweetNaCl inner layer) ──────── */
    CeePewErr_t box_err = crypto_box_decrypt(
        ctx, ascon_plain, ascon_plain_len, plaintext_out, pt_len_out);
    if (box_err != CEEPEW_OK) {
        esl->rx_auth_fail++;
        return CEEPEW_ERR_AUTH_FAIL;  /* SILENCE */
    }

    /* ── STEP 10: Ed25519 signature verify ───────────────────────── */
    CeePewErr_t sig_err = crypto_eddsa_verify(
        ctx->peer_sign_pk, *plaintext_out, *pt_len_out,
        frame + frame_len - CEEPEW_ED25519_SIG_BYTES);
    if (sig_err != CEEPEW_OK) {
        return CEEPEW_ERR_SIG_FAIL;   /* SILENCE */
    }

    esl->rx_accepted++;
    return CEEPEW_OK;
}
```

---

## SECURE BOOT + FLASH ENCRYPTION: BUILD CONFIGURATION

### `sdkconfig.defaults` (production target)

```ini
# ── Secure Boot ─────────────────────────────────────────────────────────
CONFIG_SECURE_BOOT=y
CONFIG_SECURE_BOOT_V2_ENABLED=y
CONFIG_SECURE_BOOT_SIGNING_KEY="keys/secure_boot_signing_key.pem"
CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y
# Permanently disable JTAG after first secure boot verification
CONFIG_SECURE_BOOT_JTAG_DISABLED_DURING_SECURE_BOOT=y

# ── Flash Encryption ─────────────────────────────────────────────────────
CONFIG_FLASH_ENCRYPTION_ENABLED=y
# Development mode allows re-flashing during development
# Change to FLASH_ENCRYPTION_MODE_RELEASE for production units
CONFIG_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y

# ── Stack protector ──────────────────────────────────────────────────────
CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y
CONFIG_COMPILER_CXX_EXCEPTIONS=n   # no C++ exceptions — we use error codes

# ── Watchdog (prevents hung crypto task from silently dying) ─────────────
CONFIG_ESP_TASK_WDT=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n  # UI task may idle normally
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n  # Session task may idle

# ── NVS Encryption (key in eFuse, encrypts NVS partition) ───────────────
CONFIG_NVS_ENCRYPTION=y

# ── Heap corruption detection ────────────────────────────────────────────
CONFIG_HEAP_CORRUPTION_DETECTION=y
CONFIG_HEAP_TRACING_STANDALONE=y  # detect if anything tries to malloc

# ── Minimize attack surface ──────────────────────────────────────────────
CONFIG_ESP_CONSOLE_NONE=y          # UART disabled in production
CONFIG_BOOTLOADER_LOG_LEVEL_NONE=y # no bootloader serial output
```

### Key Generation (one-time, offline, hardware security module recommended)

```bash
# Generate Secure Boot signing key (keep offline, never commit to git)
espsecure.py generate_signing_key \
    --version 2 \
    --scheme rsa3072 \
    keys/secure_boot_signing_key.pem

# Build and flash production firmware (signs automatically via sdkconfig)
idf.py build
idf.py -p /dev/ttyUSB0 flash

# After first flash with Secure Boot:
# The device burns eFuses automatically. JTAG is disabled.
# Subsequent firmware updates require the same signing key.
```

---

## ATTACK RESISTANCE MAP (Updated for ESL)

| Attack | Before ESL | After ESL |
|---|---|---|
| **Key extraction via firmware dump** | Keys in flash → fully exposed | No static keys in firmware; session keys in RAM only; flash encrypted |
| **Replay attack** | None | 64-bit WireGuard bitmap + timestamp ±15s + nonce counter |
| **MAC spoofing** | Accepted if PMK matches | Rejected by MAC lock (crypto_ct_equal) after Phase 2 |
| **MITM handshake** | Possible | Session code in HKDF salt; fingerprint display; Ed25519 key binding |
| **Flooding/DoS** | Immediate exhaustion | Token bucket + cookie mechanism + "silence on invalid" |
| **Jamming** | Full session loss | PRG channel hopping; graceful degradation |
| **UART/JTAG extraction** | Keys readable | UART disabled in production; JTAG burned via eFuse |
| **Flash chip extraction** | Plaintext firmware | AES-256 flash encryption; eFuse-held key never leaves chip |
| **Firmware modification** | Undetected | Secure Boot V2 rejects unsigned binaries |
| **Cross-session replay** | Possible | session_id in nonce ties every packet to exactly one session |
| **Partial session intercept** | Decryptable with PMK | Double-layer encryption (Ascon + crypto_box); PMK is session-derived |
| **Physical device seizure (post-session)** | Keys recoverable from RAM if power maintained | secure_zero on session end; no keys in flash/NVS |

---

## ADDITIONAL CONFIG CONSTANTS (Append to `ceepew_config.h`)

```c
/* ── ESL (ESP-NOW Security Layer) ───────────────────────────────────── */
#define CEEPEW_REPLAY_WINDOW_SIZE      64U
#define CEEPEW_TIMESTAMP_SLACK_S       15U
#define CEEPEW_NONCE_MAX_GAP           64U
#define CEEPEW_SESSION_GRACE_S         30U   /* reconnect without re-pair */
#define CEEPEW_DOS_QUEUE_THRESHOLD     6U    /* 75% of QUEUE_DEPTH=8     */
#define CEEPEW_COOKIE_ROTATE_S         120U
#define CEEPEW_COOKIE_BYTES            16U
#define CEEPEW_RATE_LIMIT_TOKENS       10U
#define CEEPEW_RATE_LIMIT_RATE_MS      100U
#define CEEPEW_MAX_RECONNECT_ATTEMPTS  5U
#define CEEPEW_HOP_CHANNELS            9U
#define CEEPEW_HOP_INTERVAL_MS         5000U
#define CEEPEW_HOP_SHIFT               6U    /* hop every 64 messages    */
#define CEEPEW_MIN_FRAME_BYTES         (sizeof(CeePewPacketHeader_t) + \
                                        CEEPEW_TAG_BYTES + \
                                        CEEPEW_ED25519_SIG_BYTES + 4U)

/* ── DoS Jamming Detection Thresholds ───────────────────────────────── */
#define CEEPEW_JAM_DETECT_WINDOW       10U   /* packets to evaluate      */
#define CEEPEW_JAM_FAIL_THRESHOLD_PCT  50U   /* >50% fail = interference */
#define CEEPEW_JAM_FEC_THRESHOLD_PCT   30U   /* >30% FEC fail            */
#define CEEPEW_JAM_RSSI_MIN_DBM        (-80) /* signal present but failing */
```

---

## UPDATED MODULE MAP (Additions from This Addendum)

```
transport/
├── transport_espnow.h/.c     ESP-NOW init, send, recv callback
├── transport_esl.h/.c        ← NEW: full ESL pipeline (this document)
├── transport_replay.h/.c     ← NEW: WireGuard anti-replay bitmap
├── transport_dos.h/.c        ← NEW: DoS mitigation, cookie, rate limit
├── transport_hop.h/.c        ← NEW: PRG channel hopping
└── transport_packet.h        packed wire structs (__attribute__((packed)))

keys/                         ← NEW directory (not committed to git)
├── .gitignore                contains: *
├── secure_boot_signing_key.pem   ← offline, hardware-protected
└── README.md                 key handling instructions

sdkconfig.defaults            production security settings
sdkconfig.debug               development settings (UART enabled)
```

---

## PROJECT MODE ADDENDUM (Append to Part 13 of Final Spec v2)

```
── ESL ADDENDUM TO PROJECT INSTRUCTIONS ──

MODULE: transport_esl
  The ESP-NOW Security Layer is the gatekeeper for all received frames.
  esl_recv() is the ONLY entry point for incoming data into the session layer.
  No other code may directly process raw ESP-NOW callback data.

SILENCE RULE (enforced, not suggested):
  Authentication failures (Ascon, Ed25519, signature) produce NO response.
  No NACK. No error packet. No log output (even in DEBUG mode).
  Only CRC and FEC failures produce NACKs (transport errors, not security).

SECURE BOOT + FLASH ENCRYPTION:
  These are enabled in sdkconfig.defaults.
  For development, use sdkconfig.debug (UART on, Dev mode flash encryption).
  NEVER commit keys/ directory to git. The .gitignore in keys/ enforces this.
  Secure Boot signing key MUST be kept offline (USB drive, not cloud storage).

PHYSICAL SECURITY NOTE:
  CEE-PEW is designed to be physically seized mid-session and yield no
  session key material, provided secure_zero() is called on power-down
  (via brownout detector ISR). Implement the brownout ISR in hal_power.c:
    • Register brownout ISR via esp_register_shutdown_handler()
    • ISR calls crypto_ctx_destroy(&g_crypto_ctx) before halt

NEW MILESTONES:
  [ ] M-ESL-1: transport_replay.c — WireGuard bitmap, unit tested
  [ ] M-ESL-2: transport_dos.c — cookie mechanism, rate limiter
  [ ] M-ESL-3: transport_hop.c — channel PRG, both devices sync
  [ ] M-ESL-4: transport_esl.c — full pipeline, integrated
  [ ] M-ESL-5: sdkconfig.defaults — Secure Boot build verified
  [ ] M-ESL-6: Flash encryption — production flash verified encrypted
  [ ] M-ESL-7: Brownout ISR — crypto_ctx_destroy on power loss
  [ ] M-ESL-8: DIAG ESL page — ESL stats visible on DIAG page 3
```

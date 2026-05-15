# CEE-PEW — Addendum A: Pin Configuration & Resource Monitor
### Derived from Numerology Calculator Architecture
**Version:** 1.0 | Extends: `CEEPEW_Architecture_v1.md`

---

## Table of Contents
1. Pin Configuration System
2. The DIAG Mode — Resource Monitor
3. Integration with Session FSM
4. DIAG Mode Display Specification
5. DIAG Module Interface
6. Struct Definitions & Constants
7. How It All Connects (Full Updated Module Map)

---

## 1. Pin Configuration System

### 1.1 Philosophy
The numerology calculator taught one important lesson: **pin assignments that are
scattered across source files become a maintenance nightmare the moment you swap
hardware**. One resistor change, one PCB revision, one breadboard rewire — and
you're doing a grep across the whole codebase.

CEE-PEW centralises *every* GPIO assignment, ADC channel, I2C address, and
hardware-specific constant into a single header: `hal/hal_pins.h`. This file is the
**only** place where physical hardware is described in code. Everything else refers
to the logical names defined here. Changing hardware = editing one file.

### 1.2 `hal/hal_pins.h` — Complete Pin Configuration

```c
#ifndef HAL_PINS_H
#define HAL_PINS_H

/*
 * ═══════════════════════════════════════════════════════════════════
 *  CEE-PEW Pin Configuration
 *  Board: ESP32-WROOM-32 (38-pin variant)
 *
 *  RULE: This is the ONLY file in the project that may contain
 *        raw GPIO numbers. All other code uses the symbolic names
 *        defined here. Never write GPIO_NUM_34 in application code.
 * ═══════════════════════════════════════════════════════════════════
 */

#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/i2c.h"

/* ── OLED Display (SSD1306, I2C) ─────────────────────────────────── */
#define CEEPEW_PIN_I2C_SDA          GPIO_NUM_21
#define CEEPEW_PIN_I2C_SCL          GPIO_NUM_22
#define CEEPEW_I2C_PORT             I2C_NUM_0
#define CEEPEW_I2C_FREQ_HZ          400000U        /* 400 kHz Fast Mode    */
#define CEEPEW_OLED_I2C_ADDR        0x3CU          /* SSD1306 default addr */
#define CEEPEW_OLED_WIDTH_PX        128U
#define CEEPEW_OLED_HEIGHT_PX       64U

/* ── Rotary Potentiometer (Analog Input) ─────────────────────────── */
#define CEEPEW_PIN_POT              GPIO_NUM_34    /* ADC1_CH6, input-only */
#define CEEPEW_ADC_CHANNEL_POT      ADC1_CHANNEL_6
#define CEEPEW_ADC_ATTEN            ADC_ATTEN_DB_11  /* 0–3.3V full range  */
#define CEEPEW_ADC_WIDTH            ADC_WIDTH_BIT_12 /* 12-bit: 0–4095     */
#define CEEPEW_ADC_MAX_RAW          4095U
#define CEEPEW_ADC_SAMPLES_PER_READ 8U             /* oversampling average */

/* ── Click Button ────────────────────────────────────────────────── */
#define CEEPEW_PIN_BUTTON           GPIO_NUM_35    /* input-only, no pullup */
#define CEEPEW_BUTTON_ACTIVE_LEVEL  0              /* active LOW (GND)      */
#define CEEPEW_BUTTON_DEBOUNCE_MS   25U

/* ── Mode Button (hold for DIAG mode) ───────────────────────────── */
/*    NOTE: Can be the same physical button as CEEPEW_PIN_BUTTON.    */
/*    DIAG is triggered by a long-press (>= CEEPEW_DIAG_HOLD_MS).   */
#define CEEPEW_DIAG_HOLD_MS         2000U          /* 2-second hold        */

/* ── Status LED (optional, single-colour) ───────────────────────── */
#define CEEPEW_PIN_STATUS_LED       GPIO_NUM_2     /* on-board LED on most */
                                                   /* ESP32 devboards       */
#define CEEPEW_LED_ACTIVE_LEVEL     1              /* active HIGH           */

/* ── RGB LED (common-cathode, 3×220Ω to 3.3V) ───────────────────── */
/*    All status is conveyed via the RGB LED and OLED exclusively.    */
/*    Managed exclusively by hal/hal_rgb.c via rgb_set_pattern().     */
/*    No other module may write these pins directly.                   */
#define CEEPEW_PIN_RGB_RED          GPIO_NUM_2     /* also onboard LED     */
#define CEEPEW_PIN_RGB_GREEN        GPIO_NUM_18
#define CEEPEW_PIN_RGB_BLUE         GPIO_NUM_23

/* ── ESP-NOW / WiFi / BLE ────────────────────────────────────────── */
/*    These use the internal radio — no external GPIO needed.         */
/*    Channel and protocol constants live here for single-source.    */
#define CEEPEW_ESPNOW_CHANNEL       1U             /* WiFi channel 1        */
#define CEEPEW_ESPNOW_RATE          WIFI_PHY_RATE_MCS7_SGI  /* 65 Mbps HT  */
#define CEEPEW_BLE_ADV_INTERVAL_MS  100U           /* BLE scan interval     */

/* ── Hardware Entropy Source ─────────────────────────────────────── */
/*    ESP32 RNG is seeded by RF subsystem thermal noise.              */
/*    No external pin needed; documented here for traceability.       */
#define CEEPEW_RNG_SOURCE           "ESP32_INTERNAL_RNG_RF_THERMAL"

/* ── I2C Pull-up Resistors ───────────────────────────────────────── */
/*    External 4.7kΩ pull-ups to 3.3V on SDA and SCL lines.          */
/*    Internal pull-ups are weak (~45kΩ) — insufficient at 400kHz.   */
/*    Document this here so hardware builder is warned.               */
#define CEEPEW_I2C_PULLUP_OHMS      4700U          /* external, not code    */

/* ── ADC Calibration ─────────────────────────────────────────────── */
/*    Use esp_adc_cal for per-chip eFuse calibration.                 */
/*    Reference voltage (nominally 1100mV internal ref at 11dB).      */
#define CEEPEW_ADC_VREF_MV          1100U

/*
 * Pin Validation Macro
 * Called once in hal_pins_validate() at boot. Ensures GPIO assignments
 * are sane — catches copy-paste errors where two peripherals share a pin.
 * Expands to a complete syntactic unit.
 */
#define CEEPEW_PINS_ASSERT_UNIQUE()                                     \
    do {                                                                \
        _Static_assert(CEEPEW_PIN_I2C_SDA != CEEPEW_PIN_I2C_SCL,      \
            "SDA and SCL must differ");                                 \
        _Static_assert(CEEPEW_PIN_POT     != CEEPEW_PIN_BUTTON,        \
            "POT and BUTTON must differ");                              \
        _Static_assert(CEEPEW_PIN_POT     != CEEPEW_PIN_I2C_SDA,       \
            "POT must not share I2C SDA");                              \
        _Static_assert(CEEPEW_PIN_BUTTON  != CEEPEW_PIN_STATUS_LED,    \
            "BUTTON and LED must differ");                              \
        _Static_assert(CEEPEW_PIN_RGB_RED   != CEEPEW_PIN_RGB_GREEN,   \
            "RGB R and G must differ");                                \
        _Static_assert(CEEPEW_PIN_RGB_GREEN  != CEEPEW_PIN_RGB_BLUE,   \
            "RGB G and B must differ");                                \
        _Static_assert(CEEPEW_PIN_RGB_RED    != CEEPEW_PIN_RGB_BLUE,   \
            "RGB R and B must differ");                                \
    } while (0)

#endif /* HAL_PINS_H */
```

### 1.3 `hal/hal_pins.c` — Runtime Validation at Boot

```c
#include "hal_pins.h"
#include "ceepew_assert.h"

/*
 * hal_pins_validate()
 * Called ONCE from app_main before any peripheral initialisation.
 * Verifies that input-only pins (34, 35 on ESP32) are not configured
 * as outputs. Detects board mismatches early.
 */
CeePewErr_t hal_pins_validate(void)
{
    /* Assertion 1: pot pin must be a valid ADC1 pin on ESP32 */
    CEEPEW_ASSERT(
        (CEEPEW_PIN_POT == GPIO_NUM_32) || (CEEPEW_PIN_POT == GPIO_NUM_33) ||
        (CEEPEW_PIN_POT == GPIO_NUM_34) || (CEEPEW_PIN_POT == GPIO_NUM_35) ||
        (CEEPEW_PIN_POT == GPIO_NUM_36) || (CEEPEW_PIN_POT == GPIO_NUM_39),
        CEEPEW_ERR_PARAM
    );

    /* Assertion 2: GPIO 34–39 are input-only — must not be used as output */
    CEEPEW_ASSERT(
        (CEEPEW_PIN_STATUS_LED != GPIO_NUM_34) &&
        (CEEPEW_PIN_STATUS_LED != GPIO_NUM_35) &&
        (CEEPEW_PIN_STATUS_LED != GPIO_NUM_36) &&
        (CEEPEW_PIN_STATUS_LED != GPIO_NUM_39),
        CEEPEW_ERR_PARAM
    );

    /* Compile-time unique-pin assertions */
    CEEPEW_PINS_ASSERT_UNIQUE();

    return CEEPEW_OK;
}
```

### 1.4 Pin Assignment Rationale Table

| Signal | GPIO | Reason |
|---|---|---|
| I2C SDA | 21 | Default ESP32 I2C SDA, hardware-mapped |
| I2C SCL | 22 | Default ESP32 I2C SCL, hardware-mapped |
| Potentiometer | 34 | ADC1_CH6; input-only pin — safe, no accidental output |
| Button | 35 | ADC1_CH7; input-only — cannot accidentally drive it HIGH |
| Status LED | 2 | On-board LED on most ESP32 devboards; easy debugging |
| RGB Red   | 2  | Shared with onboard LED; heartbeat 10 ms pulse every 2 s in IDLE |
| RGB Green | 18 | Indicates secure session / normal operation                       |
| RGB Blue  | 23 | Indicates radio activity (BLE discovery, ESP-NOW)                 |

**Why ADC1 and not ADC2 for the potentiometer?**
ADC2 is shared with the WiFi subsystem. When WiFi is active (which it will be, for
ESP-NOW), ADC2 readings become unreliable or block. ADC1 is completely independent.
This is a non-obvious ESP32 hardware quirk — documenting it here prevents a future
debugging nightmare.

---

## 2. The DIAG Mode — Resource Monitor

### 2.1 Concept

The numerology calculator had an elegant side-mode where the display switched from its
primary function to show internal state. CEE-PEW elevates this to a **full diagnostic
mode** (DIAG) accessible at any time by holding the button for `CEEPEW_DIAG_HOLD_MS`
(2 seconds). It does not interrupt an active session — crypto and transport continue
running on Core 1. DIAG only affects what Core 0 renders on the OLED.

The DIAG mode is inspired by:
- The numerology calculator's mode-switching approach
- Crypto AG's HX-63 operator panels that showed machine state
- The `htop`-style resource view that hackers actually want to see

### 2.2 DIAG Mode Pages

The potentiometer scrolls between pages in DIAG mode. The button exits DIAG and
returns to whatever the previous UI state was.

```
Page 0: System Resources      Page 1: Crypto Status
Page 2: Network / Radio        Page 3: Error Statistics
Page 4: Session Timeline       Page 5: Memory Map
```

### 2.3 Page 0 — System Resources

```
┌──────────────────────────────┐
│ ▸ SYSTEM         [DIAG 0/5] │
│ CPU0: ████░░░░  43%          │  ← Core 0 task load (UI)
│ CPU1: ██████░░  72%          │  ← Core 1 task load (Crypto)
│ HEAP: ███░░░░░  38% / 520KB  │  ← Free SRAM percentage
│ TEMP: 52°C  VBAT: 3.81V      │  ← ESP32 internal temp sensor
│ UPTIME: 00d 01h 23m 07s      │
│ TICK: 1000Hz FreeRTOS        │
└──────────────────────────────┘
```

**CPU load calculation:**
FreeRTOS provides `vTaskGetRunTimeStats()`. DIAG samples this every
`CEEPEW_DIAG_SAMPLE_MS` (1000ms) and computes percentage per core by dividing
each task's runtime by total elapsed ticks. Bar graphs are rendered as filled
block characters scaled to the display width.

**Temperature:**
ESP32 has an internal temperature sensor (`temperature_sensor_get_celsius()`
in ESP-IDF v5+). Used here for thermal awareness — if temp exceeds
`CEEPEW_TEMP_WARN_C` (70°C), a `[!TEMP]` warning is shown on all DIAG pages.

**Battery voltage:**
Read via ADC1 from a resistor divider on the battery rail
(pin `CEEPEW_PIN_VBAT_ADC`, configurable in `hal_pins.h` if added).
If not present, displays `VBAT: N/A`.

### 2.4 Page 1 — Crypto Status

```
┌──────────────────────────────┐
│ ▸ CRYPTO         [DIAG 1/5] │
│ SESSION: ● ACTIVE            │  ← or ○ IDLE
│ ALGO:  Ascon-128             │
│ KEYX:  Curve25519 + HKDF     │
│ SIG:   Ed25519               │
│ KEY FP: A3F7..B2C1           │  ← first 4 + last 4 bytes of session_key
│ NONCE: 0x000000000000004A    │  ← full 64-bit message counter
│ NONCE%: ██░░░░░░ 0.000001%   │  ← how "used up" the nonce space is
│ FWD SEC: ✓ ECDH EPHEMERAL    │
└──────────────────────────────┘
```

**Nonce percentage bar:**
`nonce_counter / CEEPEW_NONCE_HARD_LIMIT × 100`. At normal message rates
(say 100 messages/session) this will read `0.000001%` — which is intentionally
reassuring. If it ever reads above 1%, something is catastrophically wrong and
a `[!NONCE]` alarm is shown.

**Key fingerprint:**
The first 4 and last 4 bytes of `session_key` displayed as hex. Matches the
fingerprint shown during key exchange — allows manual out-of-band verification
that both devices derived the same key. Inspired directly by Signal's
"safety number" feature and the HX-63 operator verifying rotor starting positions.

### 2.5 Page 2 — Network / Radio

```
┌──────────────────────────────┐
│ ▸ NETWORK        [DIAG 2/5] │
│ PROTOCOL: ESP-NOW            │
│ CHANNEL:  1 (2.412 GHz)      │
│ PEER MAC: AA:BB:CC:DD:EE:FF  │
│ RSSI: -61 dBm  ████████░░    │  ← signal quality bar
│ TX PKT:   142  RX PKT:  139  │
│ TX BYTES: 18.4 KB            │
│ RX BYTES: 17.9 KB            │
│ BLE: SCANNING ···            │  ← animated dots if in discovery
└──────────────────────────────┘
```

**RSSI bar mapping:**
```
-30 dBm → 10/10 bars (excellent, very close)
-60 dBm →  7/10 bars (good)
-75 dBm →  5/10 bars (fair)
-90 dBm →  2/10 bars (poor)
-100dBm →  0/10 bars (no signal)
```
RSSI is obtained from the ESP-NOW receive callback's `esp_now_recv_info_t`.

### 2.6 Page 3 — Error Statistics

```
┌──────────────────────────────┐
│ ▸ ERROR STATS    [DIAG 3/5] │
│ CRC FAIL:     0              │
│ FEC CORRECT:  3  (3 pkts)   │  ← bits corrected / packets affected
│ FEC UNCORRECT:0              │
│ AUTH FAIL:    0              │
│ SIG FAIL:     0              │
│ REPLAY DET:   0              │
│ ARQ RETRIES:  5              │
│ ARQ FAIL:     0              │
│ SESSION ERR:  0              │
└──────────────────────────────┘
```

**Why this matters:** Zero `AUTH FAIL` and zero `SIG FAIL` under normal operation
is the expected state. Any non-zero value here during a session is an active security
event, not mere noise. The FEC_CORRECT counter showing small numbers is normal and
reflects radio environment quality — it's doing its job.

### 2.7 Page 4 — Session Timeline

```
┌──────────────────────────────┐
│ ▸ TIMELINE       [DIAG 4/5] │
│ BOOT:      00:00:00          │
│ DISCOVERY: 00:00:03          │
│ PAIRED:    00:00:41          │
│ KEYX DONE: 00:00:43          │
│ 1ST MSG:   00:01:02          │
│ LAST MSG:  00:07:19          │
│ NOW:       00:07:33          │
│ MSGS SENT: 12  RECV: 11      │
│ WIPE IN:   02m 27s           │
└──────────────────────────────┘
```

This page gives the operator full situational awareness of the session's lifecycle —
directly analogous to the HX-63 operator's log of machine configuration timestamps.

### 2.8 Page 5 — Memory Map

```
┌──────────────────────────────┐
│ ▸ MEMORY         [DIAG 5/5] │
│ SRAM TOTAL:  520 KB          │
│ USED:         19 KB  ██░░░░  │
│ FREE:        501 KB          │
│ HEAP FREE:   498 KB          │
│ HEAP MIN:    496 KB          │  ← low-water mark since boot
│ STACK C0:   4096B / 4096B   │
│ STACK C1:   6821B / 8192B   │
│ MSG BUF:    03/20 slots      │  ← circular buffer occupancy
│ FLASH FREE: 1.2 MB           │
└──────────────────────────────┘
```

**Stack high-water mark:** `uxTaskGetStackHighWaterMark()` for each FreeRTOS task.
If either stack is >90% used, a `[!STACK]` alarm appears on all pages. This catches
stack overflows before they cause undefined behaviour — critical for a crypto device
where stack corruption could be catastrophic.

**Heap minimum:** `xPortGetMinimumEverFreeHeapSize()`. Since CEE-PEW uses no heap
after init (by design), this number should stay flat. If it decreases, it means
something in the libraries is allocating — a coding standards violation to investigate.

---

## 3. Integration with Session FSM

### 3.1 DIAG Mode is Orthogonal to Session State

DIAG mode is a **UI layer concern only**. It does not pause, alter, or interrupt
the session FSM running on Core 1. The session FSM has no knowledge of DIAG mode.

```
Core 0 (UI)                          Core 1 (Crypto/Session)
───────────────────────────────────  ──────────────────────────────────────
normal UI rendering                  session FSM: ST_SECURE_CHAT
    │                                    │
    │ [user holds button 2s]             │ [message arrives from peer]
    ▼                                    ▼
UI enters DIAG mode                  Core 1 processes, decrypts, enqueues
    │                                    │
    │ [DIAG renders resource pages]      │ [Core 1 puts msg in UI queue]
    │                                    │
    │ [user exits DIAG]                  │
    ▼                                    ▼
UI returns to ST_SECURE_CHAT render  Core 1 continues unaffected
    │                                    │
    │ [UI dequeues pending message]      │
    ▼                                    │
Message displayed                        │
```

**FreeRTOS Queue between cores:**
```c
/* Defined in session/session_fsm.h */
extern QueueHandle_t g_ui_event_queue;   /* Core 1 → Core 0 events     */
extern QueueHandle_t g_crypto_cmd_queue; /* Core 0 → Core 1 commands   */
```

Messages arriving while in DIAG mode are queued. On DIAG exit, the UI drains
the queue and renders any pending messages. No messages are lost.

### 3.2 DIAG Entry/Exit in UI FSM

```
New UI States:
    ST_UI_DIAG_PAGE_0  (System Resources)
    ST_UI_DIAG_PAGE_1  (Crypto Status)
    ST_UI_DIAG_PAGE_2  (Network)
    ST_UI_DIAG_PAGE_3  (Error Stats)
    ST_UI_DIAG_PAGE_4  (Timeline)
    ST_UI_DIAG_PAGE_5  (Memory Map)

DIAG transitions:
    Any UI state      → ST_UI_DIAG_PAGE_0    on: long-press (>= CEEPEW_DIAG_HOLD_MS)
    ST_UI_DIAG_PAGE_N → ST_UI_DIAG_PAGE_N+1  on: pot scrolled right
    ST_UI_DIAG_PAGE_N → ST_UI_DIAG_PAGE_N-1  on: pot scrolled left
    ST_UI_DIAG_PAGE_N → [previous UI state]   on: short button click
    ST_UI_DIAG_PAGE_N → ST_UI_DIAG_PAGE_0    on: auto-timeout (CEEPEW_DIAG_TIMEOUT_S)
```

### 3.3 DIAG Timeout (Auto-Exit)

If the user enters DIAG and forgets about it, the display auto-returns to the
normal UI after `CEEPEW_DIAG_TIMEOUT_S` (30 seconds) of potentiometer inactivity.
This prevents the device being left on the DIAG screen during an active session
where the user might miss an incoming message notification.

---

## 4. DIAG Mode Display Specification

### 4.1 Layout Grid (128 × 64 pixels)

```
Row 0–7   (8px):  DIAG header bar  — page title + page indicator [N/5]
Row 8–55  (48px): Content area     — 6 lines of text at 8px per line
Row 56–63 (8px):  Status bar       — security anomaly indicators [E][!][R][T][W]
```

### 4.2 Bar Graph Rendering

Bar graphs use a fixed 8-character width (64 pixels at font width 8):
```c
/*
 * diag_render_bar()
 * Renders a proportional fill bar: "████░░░░"
 *
 * Params:
 *   value     — current value (0 to max_value inclusive)
 *   max_value — maximum value (compile-time constant for each call site)
 *   buf       — output char buffer, exactly CEEPEW_BAR_WIDTH+1 bytes
 *
 * No heap allocation. buf is caller-provided, stack-allocated.
 */
#define CEEPEW_BAR_WIDTH 8U

static CeePewErr_t diag_render_bar(uint32_t value, uint32_t max_value,
                                    char buf[CEEPEW_BAR_WIDTH + 1U])
{
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(max_value > 0U, CEEPEW_ERR_PARAM);

    /* Clamp value to max_value — loop bound is always CEEPEW_BAR_WIDTH */
    uint32_t clamped = (value > max_value) ? max_value : value;
    uint32_t filled  = (clamped * CEEPEW_BAR_WIDTH) / max_value;

    for (uint8_t i = 0U; i < CEEPEW_BAR_WIDTH; i++) {
        buf[i] = (i < (uint8_t)filled) ? '\xDB' : '\xB0'; /* ▓ and ░ */
    }
    buf[CEEPEW_BAR_WIDTH] = '\0';

    return CEEPEW_OK;
}
```

### 4.3 Page Scroll via Potentiometer in DIAG Mode

In DIAG mode, the potentiometer's 0–4095 range is divided into 6 equal zones,
one per page. The EMA-smoothed value determines the current page. This reuses
the same `InputCtx_t` and `input_update()` infrastructure from normal character
selection — no new input code needed.

```
Zone 0: ADC   0– 682 → Page 0 (System Resources)
Zone 1: ADC 683–1365 → Page 1 (Crypto Status)
Zone 2: ADC 1366–2048 → Page 2 (Network)
Zone 3: ADC 2049–2730 → Page 3 (Error Stats)
Zone 4: ADC 2731–3413 → Page 4 (Timeline)
Zone 5: ADC 3414–4095 → Page 5 (Memory Map)
```

```c
/* DIAG page index from smoothed ADC — O(1), no loop, no division at runtime */
/* Uses integer multiply to avoid division: zone = (adc * 6) / 4096          */
static uint8_t diag_page_from_adc(uint16_t smoothed_adc)
{
    uint32_t zone = ((uint32_t)smoothed_adc * 6UL) >> 12U; /* divide by 4096 */
    return (uint8_t)((zone > 5U) ? 5U : zone);             /* clamp to [0,5] */
}
```

---

## 5. DIAG Module Interface

### 5.1 `DiagCtx_t` — Diagnostic State Struct

```c
/* ui/ui_diag.h */
#ifndef UI_DIAG_H
#define UI_DIAG_H

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Additional DIAG Config Constants ──────────────────────────── */
#define CEEPEW_DIAG_PAGES           6U
#define CEEPEW_DIAG_TIMEOUT_S       30U
#define CEEPEW_DIAG_SAMPLE_MS       1000U
#define CEEPEW_TEMP_WARN_C          70
#define CEEPEW_STACK_WARN_PCT       90U
#define CEEPEW_BAR_WIDTH            8U

/* ── Error Statistics (accumulates over session lifetime) ────────── */
typedef struct {
    uint32_t crc_fail;
    uint32_t fec_corrected_bits;
    uint32_t fec_corrected_pkts;
    uint32_t fec_uncorrectable;
    uint32_t auth_fail;
    uint32_t sig_fail;
    uint32_t replay_detected;
    uint32_t arq_retries;
    uint32_t arq_fail;
    uint32_t session_errors;
} ErrorStats_t;

/* ── Session Timeline ────────────────────────────────────────────── */
typedef struct {
    uint32_t t_boot_s;
    uint32_t t_discovery_s;
    uint32_t t_paired_s;
    uint32_t t_keyx_done_s;
    uint32_t t_first_msg_s;
    uint32_t t_last_msg_s;
    uint32_t msgs_sent;
    uint32_t msgs_received;
} SessionTimeline_t;

/* ── System Snapshot (sampled every CEEPEW_DIAG_SAMPLE_MS) ──────── */
typedef struct {
    uint8_t  cpu0_load_pct;      /* Core 0 utilisation 0–100   */
    uint8_t  cpu1_load_pct;      /* Core 1 utilisation 0–100   */
    uint32_t heap_free_bytes;
    uint32_t heap_min_bytes;     /* low-water mark             */
    uint16_t stack_core0_free;   /* words remaining            */
    uint16_t stack_core1_free;
    int32_t  temp_celsius;       /* internal sensor reading    */
    uint32_t vbat_mv;            /* battery voltage in mV      */
    uint32_t uptime_s;
    uint8_t  msg_buf_used;       /* circular buffer occupancy  */
    uint32_t flash_free_bytes;
    int8_t   rssi_dbm;           /* last received packet RSSI  */
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint8_t  peer_mac[CEEPEW_DEVICE_ID_BYTES];
} SysSnapshot_t;

/* ── DIAG Context ────────────────────────────────────────────────── */
typedef struct {
    uint8_t           current_page;
    uint32_t          last_activity_s;    /* for auto-timeout           */
    uint32_t          last_sample_ms;     /* for periodic snapshot      */
    SysSnapshot_t     snapshot;
    ErrorStats_t      errors;
    SessionTimeline_t timeline;
    bool              active;             /* true = DIAG mode shown     */
    bool              temp_warn;          /* true = overtemp            */
    bool              stack_warn;         /* true = stack near limit    */
    bool              nonce_warn;         /* true = nonce > 50% used    */
} DiagCtx_t;

extern DiagCtx_t g_diag_ctx;

/* ── DIAG API ────────────────────────────────────────────────────── */
CeePewErr_t diag_init(DiagCtx_t *ctx);
CeePewErr_t diag_enter(DiagCtx_t *ctx, uint32_t now_s);
CeePewErr_t diag_exit(DiagCtx_t *ctx);
CeePewErr_t diag_update(DiagCtx_t *ctx, uint16_t smoothed_adc, uint32_t now_ms);
CeePewErr_t diag_render(const DiagCtx_t *ctx);

/* Called by Core 1 whenever a transport event occurs */
void diag_record_crc_fail(DiagCtx_t *ctx);
void diag_record_fec_correct(DiagCtx_t *ctx, uint8_t bits_fixed);
void diag_record_fec_uncorrectable(DiagCtx_t *ctx);
void diag_record_auth_fail(DiagCtx_t *ctx);
void diag_record_arq_retry(DiagCtx_t *ctx);
void diag_record_msg_sent(DiagCtx_t *ctx, uint32_t now_s);
void diag_record_msg_received(DiagCtx_t *ctx, uint32_t now_s);

#endif /* UI_DIAG_H */
```

---

## 6. Struct Definitions & Constants (Additional to Architecture v1)

### 6.1 Additional `ceepew_config.h` entries (append to existing)

```c
/* ── DIAG Mode ──────────────────────────────────────────────────────── */
#define CEEPEW_DIAG_PAGES           6U
#define CEEPEW_DIAG_TIMEOUT_S       30U
#define CEEPEW_DIAG_SAMPLE_MS       1000U
#define CEEPEW_TEMP_WARN_C          70
#define CEEPEW_STACK_WARN_PCT       90U
#define CEEPEW_NONCE_WARN_PCT       50U     /* warn when 50% of nonce space used  */

/* ── Pin Config (numeric literals ONLY appear in hal_pins.h) ────────── */
/* No GPIO numbers here — they live exclusively in hal_pins.h           */

/* ── Battery Monitoring ─────────────────────────────────────────────── */
#define CEEPEW_VBAT_DIV_R1          100000U  /* resistor divider R1 (ohms) */
#define CEEPEW_VBAT_DIV_R2          100000U  /* resistor divider R2 (ohms) */
/* Vbat = Vadc × (R1 + R2) / R2 = Vadc × 2 in this symmetric config     */
#define CEEPEW_VBAT_SCALE_NUM       2U
#define CEEPEW_VBAT_SCALE_DEN       1U
#define CEEPEW_VBAT_LOW_MV          3400U    /* low battery warning         */
#define CEEPEW_VBAT_CRITICAL_MV     3200U    /* critical — initiate shutdown */
```

### 6.2 Status Bar Anomaly Flags (encoded as bitmask for compact storage)

```c
/* ui/ui_renderer.h */
typedef enum {
    CEEPEW_STATUS_CLEAR     = 0x00U,
    CEEPEW_STATUS_FEC       = 0x01U,  /* [E] FEC correction fired         */
    CEEPEW_STATUS_NONCE     = 0x02U,  /* [!] Nonce approaching limit      */
    CEEPEW_STATUS_REPLAY    = 0x04U,  /* [R] Replay attempt detected      */
    CEEPEW_STATUS_TTL_LOW   = 0x08U,  /* [T] Session TTL < 10%            */
    CEEPEW_STATUS_WIPE_SOON = 0x10U,  /* [W] Message wipe < 60s           */
    CEEPEW_STATUS_TEMP      = 0x20U,  /* [H] Overtemperature              */
    CEEPEW_STATUS_VBAT      = 0x40U,  /* [B] Low battery                  */
    CEEPEW_STATUS_STACK     = 0x80U,  /* [S] Stack near overflow          */
} StatusFlags_t;
```

---

## 7. How It All Connects — Updated Module Map

```
ceepew/
│
├── hal/                        Hardware Abstraction Layer
│   ├── hal_pins.h              ◄── ALL GPIO/ADC/I2C constants here ONLY
│   ├── hal_pins.c              Boot-time pin validation
│   ├── hal_oled.h/.c           SSD1306 driver wrapper
│   ├── hal_input.h/.c          Potentiometer + button driver
│   ├── hal_radio.h/.c          ESP-NOW wrapper
│   ├── hal_rng.h/.c            ESP32 hardware RNG wrapper
│   ├── hal_adc.h/.c            ADC1 init + calibrated read + oversampling
│   ├── hal_temp.h/.c           Internal temperature sensor
│   ├── hal_vbat.h/.c           Battery ADC read + scaling
│   └── hal_timer.h/.c          FreeRTOS timer abstractions
│
├── crypto/                     (unchanged from Architecture v1)
│
├── ecc/                        (unchanged from Architecture v1)
│
├── compress/                   (unchanged from Architecture v1)
│
├── transport/                  (unchanged from Architecture v1)
│
├── session/                    (unchanged from Architecture v1)
│
├── ui/
│   ├── ui_fsm.h/.c             UI state machine (now includes DIAG states)
│   ├── ui_renderer.h/.c        OLED frame renderer
│   ├── ui_input.h/.c           Smoothed potentiometer + button
│   ├── ui_animator.h/.c        Animation frame sequencer
│   ├── ui_feedback.h/.c        HX-63 style feedback panel
│   └── ui_diag.h/.c            ◄── NEW: DIAG mode, all 6 pages, DiagCtx_t
│
├── config/
│   ├── ceepew_config.h         All #define constants (incl. DIAG additions)
│   └── ceepew_assert.h         Assertion macros + recovery
│
└── main.c                      Core 0/1 task launch + hal_pins_validate()
```

---

## 8. Boot Sequence (Updated with Pin Validation & DIAG Init)

```c
/* main.c — app_main() */
void app_main(void)
{
    CeePewErr_t err;

    /* ── Phase 0: Hardware validation (before anything else) ── */
    err = hal_pins_validate();
    CEEPEW_ASSERT(err == CEEPEW_OK, /* void context: log and halt */ );

    /* ── Phase 1: Peripheral init ───────────────────────────── */
    err = hal_adc_init();   CEEPEW_ASSERT(err == CEEPEW_OK, );
    err = hal_oled_init();  CEEPEW_ASSERT(err == CEEPEW_OK, );
    err = hal_radio_init(); CEEPEW_ASSERT(err == CEEPEW_OK, );
    err = hal_rng_init();   CEEPEW_ASSERT(err == CEEPEW_OK, );

    /* ── Phase 2: Subsystem init (static memory only) ──────── */
    err = crypto_ctx_init(&g_crypto_ctx);          CEEPEW_ASSERT(err == CEEPEW_OK, );
    err = diag_init(&g_diag_ctx);                  CEEPEW_ASSERT(err == CEEPEW_OK, );
    err = session_fsm_init();                      CEEPEW_ASSERT(err == CEEPEW_OK, );

    /* ── Phase 3: Launch FreeRTOS tasks ─────────────────────── */
    /* Core 0: UI task (renderer + input) */
    xTaskCreatePinnedToCore(task_ui, "UI",
        CEEPEW_CORE0_STACK_BYTES, NULL, 5, NULL, 0);

    /* Core 1: Crypto/session task */
    xTaskCreatePinnedToCore(task_session, "SESSION",
        CEEPEW_CORE1_STACK_BYTES, NULL, 5, NULL, 1);

    /* app_main returns — FreeRTOS scheduler takes over */
    /* No heap allocation has occurred beyond FreeRTOS internals */
}
```

---

## 9. Milestone Updates (Addendum Items)

| Milestone | Target | Test Criteria |
|---|---|---|
| M0-A | `hal_pins.h` complete | `_Static_assert` catches any duplicate-pin config |
| M0-B | `hal_pins_validate()` | Boot fails gracefully if ADC2 pin used for pot |
| M1-A | `hal_adc.c` — oversampled read | 8-sample average stable, within ±10 ADC units |
| M1-B | `hal_temp.c` | Temperature reads within ±5°C of ambient |
| M1-C | `hal_vbat.c` | Voltage matches multimeter reading ±50mV |
| M14-A | `ui_diag.c` — all 6 pages render | All pages display without OLED glitch |
| M14-B | DIAG entry/exit | Long-press enters DIAG; short-press exits; session unaffected |
| M14-C | Error stats accumulate | Inject CRC error; DIAG page 3 shows count = 1 |
| M14-D | Auto-timeout | DIAG exits to previous screen after 30s inactivity |
| M14-E | Overtemp warning | Simulate temp > 70°C; `[H]` flag appears on status bar |

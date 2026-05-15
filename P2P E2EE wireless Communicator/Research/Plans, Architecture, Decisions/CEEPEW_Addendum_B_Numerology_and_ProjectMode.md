# CEE-PEW — Addendum B: Numerology-Derived Patterns & Project Mode Instructions
### Source analysis of numerology-calculator → CEE-PEW adaptations
**Version:** 1.0 | Extends: Architecture v1 + Addendum A

---

## Part 1 — What We're Porting and Why

### 1.1 What the Numerology Code Got Right

Reading the actual source reveals several patterns that are cleaner than what
was previously designed. Each one is adopted, adapted, or superseded below.

---

### 1.2 Pattern 1 — Dedicated Hardware Switch for Mode Toggle

**Numerology source:**
```cpp
constexpr uint8_t PIN_MONITOR_SWITCH = 5;
// In setup():
pinMode(PIN_MONITOR_SWITCH, INPUT_PULLUP);
// In loop():
const bool monitorMode = (digitalRead(PIN_MONITOR_SWITCH) == LOW);
```

**What it does:** A physical SPDT (or SPST-NO) toggle switch on GPIO 5, pulled
to GND when flipped. `INPUT_PULLUP` means the line floats HIGH at rest. No
debouncing needed — switch state is polled every loop, not edge-triggered.

**Why it's better than the long-press design in Addendum A:**
The long-press mechanic (hold button 2 seconds → enter DIAG) requires the
user's full attention and is easy to accidentally trigger during message
composition. A dedicated physical switch is unambiguous: flip it, mode changes,
flip it back, mode restores. The Crypto AG HX-63 used dedicated physical selectors
for exactly this reason — operator intent must be unambiguous on a crypto device.

**CEE-PEW adaptation:**

```c
/* hal/hal_pins.h — add to existing pin block */

/* ── DIAG Mode Hardware Switch ────────────────────────────────────── */
/*    SPST-NO switch to GND. INPUT_PULLUP. Active LOW.                */
/*    Flipping this switch INSTANTLY enters/exits DIAG mode.          */
/*    No long-press required. No debounce — polled every loop.        */
#define CEEPEW_PIN_DIAG_SWITCH      GPIO_NUM_5
#define CEEPEW_DIAG_SWITCH_ACTIVE   0   /* LOW = DIAG active          */
```

```c
/* hal/hal_input.h — extend InputCtx_t */
typedef struct {
    /* ... existing fields ... */
    bool     diag_switch_active;   /* true = DIAG switch is flipped  */
} InputCtx_t;

/* hal/hal_input.c — read switch in input_update() */
static bool read_diag_switch(void)
{
    return (gpio_get_level(CEEPEW_PIN_DIAG_SWITCH) == CEEPEW_DIAG_SWITCH_ACTIVE);
}
```

```c
/* session/session_fsm.c — DIAG entry in UI task loop */
/*
 * DIAG switch is polled once per UI loop tick (Core 0).
 * No state machine transition needed — it's a direct render override.
 * Core 1 session FSM is completely unaware of DIAG switch state.
 */
static void ui_task_loop(void *arg)
{
    InputCtx_t *input = (InputCtx_t *)arg;

    for (;;) {   /* FreeRTOS task — runs forever */
        CeePewErr_t err = input_update(input, adc_read(), button_read(),
                                        xTaskGetTickCount());
        CEEPEW_ASSERT(err == CEEPEW_OK, /* log and continue */);

        if (input->diag_switch_active) {
            /* DIAG mode — render resource monitor, do NOT touch session FSM */
            err = diag_update(&g_diag_ctx, input->smoothed_adc,
                               pdTICKS_TO_MS(xTaskGetTickCount()));
            CEEPEW_ASSERT(err == CEEPEW_OK, /* log */);
            err = diag_render(&g_diag_ctx);
            CEEPEW_ASSERT(err == CEEPEW_OK, /* log */);
        } else {
            /* Normal session UI */
            err = ui_fsm_tick(&g_ui_ctx, input);
            CEEPEW_ASSERT(err == CEEPEW_OK, /* log */);
        }

        vTaskDelay(pdMS_TO_TICKS(CEEPEW_UI_LOOP_DELAY_MS));
    }
}
```

**Add to `ceepew_config.h`:**
```c
#define CEEPEW_UI_LOOP_DELAY_MS     30U    /* matches numerology LOOP_DELAY_MS */
```

---

### 1.3 Pattern 2 — CPU Load via Loop Timing

**Numerology source:**
```cpp
const uint32_t loopBusyMicros  = micros() - loopStartMicros;
const uint32_t loopPeriodMicros = loopBusyMicros + (LOOP_DELAY_MS * 1000UL);
metrics.cpuLoadPct = (loopBusyMicros * 100UL) / loopPeriodMicros;
metrics.loopRateHz = 1000000UL / loopPeriodMicros;
```

**What it does:** Measures how much of each loop period is actual work vs. idle
delay. Elegant because it requires no RTOS-specific APIs — just timestamps.
`cpuLoadPct` = fraction of loop period spent doing real work × 100.

**CEE-PEW adaptation:**
Applied separately per core since we run two FreeRTOS tasks pinned to each core.
Core 0 (UI task) uses the numerology pattern directly. Core 1 (session task) uses
`vTaskGetRunTimeStats()` for higher precision since its workload is irregular
(crypto operations are bursty, not constant).

```c
/* ui/ui_diag.c — Core 0 CPU load measurement */

typedef struct {
    uint32_t loop_start_us;     /* micros() at top of UI task loop  */
    uint32_t loop_busy_us;      /* work time before vTaskDelay()    */
    uint32_t loop_period_us;    /* busy + delay = total period       */
    uint8_t  cpu0_load_pct;     /* derived load percentage          */
    uint32_t loop_rate_hz;      /* loops per second                 */
} LoopTiming_t;

/*
 * diag_sample_loop_timing()
 * Called ONCE at the top of the UI task loop, before any work.
 * Returns the loop timing struct for use at the bottom of the loop.
 *
 * Usage pattern (matches numerology source):
 *   LoopTiming_t t = diag_sample_loop_timing();
 *   ... do all UI work ...
 *   diag_finalise_loop_timing(&t, CEEPEW_UI_LOOP_DELAY_MS);
 *   ... vTaskDelay() ...
 */
CeePewErr_t diag_sample_loop_timing(LoopTiming_t *t)
{
    CEEPEW_ASSERT(t != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(esp_timer_get_time() >= 0, CEEPEW_ERR_PARAM);

    t->loop_start_us  = (uint32_t)esp_timer_get_time();
    t->loop_busy_us   = 0U;
    t->loop_period_us = 0U;
    t->cpu0_load_pct  = 0U;
    t->loop_rate_hz   = 0U;

    return CEEPEW_OK;
}

CeePewErr_t diag_finalise_loop_timing(LoopTiming_t *t, uint32_t delay_ms)
{
    CEEPEW_ASSERT(t != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(delay_ms > 0U, CEEPEW_ERR_PARAM);

    t->loop_busy_us   = (uint32_t)esp_timer_get_time() - t->loop_start_us;
    t->loop_period_us = t->loop_busy_us + (delay_ms * 1000U);

    /* Clamp: busy cannot exceed period */
    if (t->loop_busy_us > t->loop_period_us) {
        t->loop_busy_us = t->loop_period_us;
    }

    t->cpu0_load_pct = (uint8_t)((t->loop_busy_us * 100U) / t->loop_period_us);
    t->loop_rate_hz  = (t->loop_period_us > 0U)
                       ? (1000000U / t->loop_period_us)
                       : 0U;

    return CEEPEW_OK;
}
```

---

### 1.4 Pattern 3 — SystemMetrics Struct (Ported & Extended)

**Numerology source:**
```cpp
struct SystemMetrics {
  int      cpuLoadPct;
  uint32_t heapFreeBytes;   uint32_t heapUsedBytes;  uint32_t heapTotalBytes;
  uint32_t psramFreeBytes;  uint32_t psramUsedBytes; uint32_t psramTotalBytes;
  uint32_t loopRateHz;
  uint32_t uptimeSeconds;
};
```

**CEE-PEW adaptation (replaces SysSnapshot_t from Addendum A):**

```c
/* ui/ui_diag.h — replaces the earlier SysSnapshot_t */

typedef struct {
    /* ── Core 0 (UI task) — from loop timing ─────────────────────── */
    uint8_t  cpu0_load_pct;         /* loop-timing method           */
    uint32_t loop_rate_hz;          /* UI loops per second          */

    /* ── Core 1 (Session task) — from vTaskGetRunTimeStats ──────── */
    uint8_t  cpu1_load_pct;         /* runtime stats method         */

    /* ── Heap (internal SRAM) ────────────────────────────────────── */
    uint32_t heap_free_bytes;
    uint32_t heap_used_bytes;
    uint32_t heap_total_bytes;
    uint32_t heap_min_ever_bytes;   /* low-water mark               */

    /* ── PSRAM (if present — some ESP32 modules have 4/8MB) ─────── */
    uint32_t psram_free_bytes;
    uint32_t psram_used_bytes;
    uint32_t psram_total_bytes;

    /* ── Stack high-water marks ──────────────────────────────────── */
    uint16_t stack_core0_free_words;
    uint16_t stack_core1_free_words;

    /* ── Runtime ─────────────────────────────────────────────────── */
    uint32_t uptime_s;
    int32_t  temp_celsius;          /* internal sensor              */
    uint32_t vbat_mv;               /* battery voltage              */

    /* ── Flash ───────────────────────────────────────────────────── */
    uint32_t flash_free_bytes;
    uint8_t  flash_used_pct;

    /* ── Network (ESP-NOW) ───────────────────────────────────────── */
    int8_t   rssi_dbm;
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint8_t  peer_mac[CEEPEW_DEVICE_ID_BYTES];

    /* ── Reset reason (from numerology Page 3 pattern) ───────────── */
    esp_reset_reason_t reset_reason;
} SysSnapshot_t;

/*
 * diag_build_snapshot()
 * Builds a SysSnapshot_t from ESP-IDF system APIs.
 * Called once per CEEPEW_DIAG_SAMPLE_MS in the UI task.
 * All fields populated from static queries — no heap allocation.
 */
CeePewErr_t diag_build_snapshot(SysSnapshot_t *snap,
                                 const LoopTiming_t *loop_timing,
                                 const DiagCtx_t *diag);
```

```c
/* ui/ui_diag.c — implementation */

CeePewErr_t diag_build_snapshot(SysSnapshot_t *snap,
                                 const LoopTiming_t *loop_timing,
                                 const DiagCtx_t *diag)
{
    CEEPEW_ASSERT(snap != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(loop_timing != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(diag != NULL, CEEPEW_ERR_NULL_PTR);

    /* Core 0 load from loop timing (numerology pattern) */
    snap->cpu0_load_pct      = loop_timing->cpu0_load_pct;
    snap->loop_rate_hz       = loop_timing->loop_rate_hz;

    /* Heap — maps directly from numerology SystemMetrics */
    snap->heap_free_bytes    = esp_get_free_heap_size();
    snap->heap_total_bytes   = esp_get_minimum_free_heap_size()
                               + (heap_caps_get_total_size(MALLOC_CAP_DEFAULT)
                                  - heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    snap->heap_used_bytes    = (snap->heap_total_bytes > snap->heap_free_bytes)
                               ? (snap->heap_total_bytes - snap->heap_free_bytes)
                               : 0U;
    snap->heap_min_ever_bytes = esp_get_minimum_free_heap_size();

    /* PSRAM (ported from numerology getPsramSize() pattern) */
    snap->psram_total_bytes  = esp_spiram_get_size();
    snap->psram_free_bytes   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snap->psram_used_bytes   = (snap->psram_total_bytes > snap->psram_free_bytes)
                               ? (snap->psram_total_bytes - snap->psram_free_bytes)
                               : 0U;

    /* Stack high-water marks */
    snap->stack_core0_free_words = uxTaskGetStackHighWaterMark(g_task_ui);
    snap->stack_core1_free_words = uxTaskGetStackHighWaterMark(g_task_session);

    /* Runtime */
    snap->uptime_s           = (uint32_t)(esp_timer_get_time() / 1000000LL);
    snap->reset_reason       = esp_reset_reason();  /* numerology Page 3 */

    /* Temperature — numerology used analogRead(36) which is approximate.
     * CEE-PEW uses the proper temperature_sensor API (ESP-IDF v5+).    */
    temperature_sensor_get_celsius(g_temp_sensor, (float*)&snap->temp_celsius);

    return CEEPEW_OK;
}
```

---

### 1.5 Pattern 4 — Page Selection via Potentiometer Division

**Numerology source:**
```cpp
monitorPage = (cursorIdx * 4) / TOTAL_ITEMS;
```

**What it does:** Maps the cursor's position within `TOTAL_ITEMS` (52) to one of
4 pages. The pot range is divided proportionally — moving from 0 to full sweep
traverses all 4 pages in order.

**CEE-PEW adaptation (6 DIAG pages, uses smoothed ADC not cursor index):**
In CEE-PEW, DIAG mode works independently of the charset cursor. When the DIAG
switch is flipped, the pot's full 0–4095 range directly maps to 6 pages.

The numerology formula `(cursorIdx * N) / TOTAL_ITEMS` is equivalent to
`(adc * N) / ADC_MAX` when the cursor is linearly mapped from ADC. CEE-PEW
uses the ADC value directly, avoiding the intermediate cursor step:

```c
/* Ported from numerology — same divide-then-truncate approach, extended to 6 pages */
/* No runtime division: replaced with multiply-then-shift (>> 12 = divide by 4096)  */
static uint8_t diag_page_from_adc(uint16_t smoothed_adc)
{
    /* (adc * 6) / 4096 — shift avoids division, still statically bounded */
    uint32_t zone = ((uint32_t)smoothed_adc * 6UL) >> 12U;
    return (uint8_t)((zone > 5U) ? 5U : zone);  /* clamp: always terminates */
}
```

---

### 1.6 Pattern 5 — `_drawMetricBar()` (Direct Port, C-adapted)

**Numerology source:**
```cpp
void DisplayManager::_drawMetricBar(int x, int y, int width,
                                    const char* label, int pct,
                                    const char* valueText) {
    const int clampedPct = (pct < 0) ? 0 : ((pct > 100) ? 100 : pct);
    const int fillW = (width * clampedPct) / 100;
    _oled.drawRect(x, barY, width, barH, SSD1306_WHITE);
    if (fillW > 2) { _oled.fillRect(...); }
}
```

**CEE-PEW C adaptation:**

```c
/* ui/ui_renderer.c */

/*
 * ui_draw_metric_bar()
 * Direct port of numerology _drawMetricBar() to C.
 * Renders: label + value text on one line, filled progress bar below.
 *
 * Params validated by assertion. No heap. Stack-allocated label copies.
 */
#define CEEPEW_METRIC_BAR_H  6U

CeePewErr_t ui_draw_metric_bar(uint8_t x, uint8_t y, uint8_t width,
                                const char *label, uint8_t pct,
                                const char *value_text)
{
    CEEPEW_ASSERT(label != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(value_text != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(width > 2U, CEEPEW_ERR_PARAM);

    /* Clamp percentage — same as numerology */
    uint8_t clamped = (pct > 100U) ? 100U : pct;
    uint8_t bar_y   = y + 9U;
    uint8_t fill_w  = (uint8_t)((width * clamped) / 100U);

    /* Label + value text on first line */
    oled_set_cursor(x, y);
    oled_print(label);
    oled_print(" ");
    oled_print(value_text);

    /* Outline rectangle */
    oled_draw_rect(x, bar_y, width, CEEPEW_METRIC_BAR_H);

    /* Fill rectangle — only if fill_w > 2 (numerology guard, prevents artifacts) */
    if (fill_w > 2U) {
        oled_fill_rect((uint8_t)(x + 1U), (uint8_t)(bar_y + 1U),
                       (uint8_t)(fill_w - 2U),
                       (uint8_t)(CEEPEW_METRIC_BAR_H - 2U));
    }

    return CEEPEW_OK;
}
```

---

### 1.7 Pattern 6 — Page Dots (Direct Port)

**Numerology source:**
```cpp
void DisplayManager::_drawPageDots(int cursorIdx) {
    const int totalPages = (TOTAL_ITEMS + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    const int page = cursorIdx / ITEMS_PER_PAGE;
    for (int p = 0; p < totalPages; ++p) {
        if (p == page) { _oled.fillCircle(...); }
        else           { _oled.drawCircle(...); }
    }
}
```

**CEE-PEW C adaptation (for DIAG pages):**

```c
/* ui/ui_renderer.c */

#define CEEPEW_DOT_RADIUS   1U
#define CEEPEW_DOT_SPACING  8U      /* pixels between dot centres */
#define CEEPEW_DOT_Y        62U     /* bottom of 64px display     */

CeePewErr_t ui_draw_page_dots(uint8_t current_page, uint8_t total_pages)
{
    CEEPEW_ASSERT(total_pages > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(current_page < total_pages, CEEPEW_ERR_PARAM);

    /* Centre the dot row — same geometry as numerology */
    uint8_t total_width = (uint8_t)((total_pages - 1U) * CEEPEW_DOT_SPACING);
    uint8_t base_x      = (uint8_t)((CEEPEW_OLED_WIDTH - total_width) / 2U);

    /* Fixed upper bound: total_pages <= CEEPEW_DIAG_PAGES (6) */
    for (uint8_t p = 0U; p < total_pages; p++) {
        uint8_t cx = (uint8_t)(base_x + p * CEEPEW_DOT_SPACING);
        if (p == current_page) {
            oled_fill_circle(cx, CEEPEW_DOT_Y, CEEPEW_DOT_RADIUS);
        } else {
            oled_draw_circle(cx, CEEPEW_DOT_Y, CEEPEW_DOT_RADIUS);
        }
    }

    return CEEPEW_OK;
}
```

---

### 1.8 Pattern 7 — Reset Reason Display (Numerology Page 3, Ported)

**Numerology source:**
```cpp
esp_reset_reason_t reason = esp_reset_reason();
if      (reason == ESP_RST_POWERON)  resetReason = "Power-on";
else if (reason == ESP_RST_SW)       resetReason = "SW Reset";
else if (reason == ESP_RST_PANIC)    resetReason = "Panic";
else if (reason == ESP_RST_TASK_WDT) resetReason = "Task WDT";
else                                  resetReason = "Unknown";
```

**CEE-PEW adaptation — security-relevant extension:**
For a crypto device, reset reason is a security signal, not just a debug curiosity.
`ESP_RST_PANIC` on a crypto device should be logged and flagged. `ESP_RST_TASK_WDT`
means Core 1's crypto task deadlocked — a potential denial-of-service condition.

```c
/* ui/ui_diag.c */

static const char *diag_reset_reason_str(esp_reset_reason_t r)
{
    /* No loop, no allocation — direct switch to string literal */
    switch (r) {
        case ESP_RST_POWERON:   return "Power-on";
        case ESP_RST_SW:        return "SW Reset";
        case ESP_RST_PANIC:     return "PANIC ⚠";    /* flag visually */
        case ESP_RST_TASK_WDT:  return "Task WDT ⚠"; /* flag visually */
        case ESP_RST_INT_WDT:   return "Int WDT ⚠";
        case ESP_RST_BROWNOUT:  return "Brownout ⚠";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "Unknown";
    }
}
```

---

### 1.9 Pattern 8 — DEBUG_SERIAL Guard (Adopted Verbatim)

**Numerology source:**
```cpp
#ifdef DEBUG_SERIAL
  Serial.printf("[DBG] Input: %s -> Result: %d\n", inputStr.c_str(), finalResult);
#endif
```

**CEE-PEW adoption — with security extension:**
CEE-PEW uses the same guard but adds an extra rule: **key material and plaintext
are NEVER printed, even under `DEBUG_SERIAL`**. The guard is renamed to clarify.

```c
/* config/ceepew_config.h */
/*
 * CEEPEW_DEBUG_SERIAL — enable serial diagnostic output.
 *
 * SECURITY RULE: Even when enabled, the following are NEVER printed:
 *   - session_key, ecdh_sk, sign_sk, sign_pk (any key material)
 *   - plaintext message content
 *   - peer device MAC address in pairing logs
 * Only timing, state transitions, error codes, and non-sensitive stats.
 */
/* #define CEEPEW_DEBUG_SERIAL */   /* COMMENTED OUT for production builds */

#ifdef CEEPEW_DEBUG_SERIAL
    #define CEEPEW_LOG(fmt, ...) \
        do { ESP_LOGI("CEEPEW", fmt, ##__VA_ARGS__); } while(0)
#else
    #define CEEPEW_LOG(fmt, ...) do {} while(0)   /* compiled away entirely */
#endif
```

---

### 1.10 Pattern 9 — OLED Fallback Address

**Numerology source:**
```cpp
constexpr uint8_t OLED_ADDR          = 0x3C;
constexpr uint8_t OLED_ADDR_FALLBACK = 0x3D;
// In begin():
if (_oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))          { return true; }
if (_oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_FALLBACK)) { return true; }
return false;
```

**CEE-PEW adoption:** Already in `hal_pins.h` as `CEEPEW_OLED_I2C_ADDR = 0x3C`.
Add `CEEPEW_OLED_I2C_ADDR_FALLBACK = 0x3D` and implement the two-attempt init:

```c
/* hal/hal_oled.c */
CeePewErr_t hal_oled_init(void)
{
    /* Attempt primary address */
    if (ssd1306_init(CEEPEW_OLED_I2C_ADDR) == CEEPEW_OK) {
        CEEPEW_LOG("OLED init OK at 0x%02X", CEEPEW_OLED_I2C_ADDR);
        return CEEPEW_OK;
    }

    CEEPEW_LOG("OLED 0x%02X failed, trying fallback 0x%02X",
               CEEPEW_OLED_I2C_ADDR, CEEPEW_OLED_I2C_ADDR_FALLBACK);

    /* Assertion: if primary fails, fallback must succeed */
    CeePewErr_t err = ssd1306_init(CEEPEW_OLED_I2C_ADDR_FALLBACK);
    CEEPEW_ASSERT(err == CEEPEW_OK, CEEPEW_ERR_PARAM);

    return CEEPEW_OK;
}
```

---

### 1.11 What We're NOT Porting (and Why)

| Numerology Pattern | Why Not Ported |
|---|---|
| `Arduino String` class | Heap-backed, forbidden by coding standards. All strings in CEE-PEW use `char[]` with fixed bounds. |
| `analogSetPinAttenuation(ADC_11db)` Arduino API | Using ESP-IDF `adc1_config_channel_atten()` directly — more precise, same effect. |
| `analogReadResolution(12)` Arduino API | ESP-IDF `adc1_config_width(ADC_WIDTH_BIT_12)` is the equivalent. |
| `millis()` | Using `esp_timer_get_time()` (microsecond resolution) or FreeRTOS `xTaskGetTickCount()`. |
| `WiFi.getMode()` Arduino API | ESP-NOW does not use the WiFi stack in station/AP mode. `esp_wifi_get_mode()` used directly. |
| Loop-based `delay(LOOP_DELAY_MS)` | `vTaskDelay(pdMS_TO_TICKS(...))` in FreeRTOS — yields CPU properly instead of busy-waiting. |
| Single-core `loop()` pattern | CEE-PEW is dual-core pinned. No Arduino `loop()`. |
| Heap-allocated `SystemMetrics metrics{}` | CEE-PEW uses static `SysSnapshot_t` — same fields, no heap. |

---

## Part 2 — Updated DIAG Pages (Numerology-Informed Revision)

The 6 DIAG pages are revised to match the structure proven by the numerology
monitor (4 pages → 6 pages, with CEE-PEW-specific additions).

```
Page 0: CPU & Memory       (≈ numerology Page 0 — direct adaptation)
Page 1: Memory Detail      (≈ numerology Page 1 — heap breakdown)
Page 2: Crypto Status      (CEE-PEW specific — no numerology equivalent)
Page 3: Network / ESP-NOW  (≈ numerology Page 2 — connectivity, adapted)
Page 4: Runtime Info       (≈ numerology Page 3 — uptime, reset, temp)
Page 5: Error Statistics   (CEE-PEW specific — security event counters)
```

Page dot rendering at y=62 using `ui_draw_page_dots()` as designed above.
Layout: 8px header | 48px content (6 lines × 8px) | 8px status bar.

---

## Part 3 — Project Mode Instructions for Claude

This section is the **complete system prompt** to paste into a Claude Project
when continuing CEE-PEW development. It is designed to be copied verbatim.

---

```
═══════════════════════════════════════════════════════════════════════════
CEE-PEW PROJECT INSTRUCTIONS
Cryptographic End-to-End Peer-to-peer Encrypted Wireless Communicator
═══════════════════════════════════════════════════════════════════════════

## Identity

You are the principal engineer and code reviewer for the CEE-PEW project.
You have full context of the architecture, coding standards, and design
decisions documented in:
  • CEEPEW_Architecture_v1.md
  • CEEPEW_Addendum_PinConfig_ResourceMonitor.md (Addendum A)
  • CEEPEW_Addendum_B_Numerology_and_ProjectMode.md (Addendum B — this document)

Never contradict decisions made in these documents without explicitly flagging
the deviation and explaining why it is an improvement.

---

## Hardware Context

- MCU: ESP32-WROOM-32 (dual-core Xtensa LX6, 240 MHz, 520 KB SRAM)
- Display: SSD1306 OLED 0.96", 128×64, I2C at 0x3C (fallback 0x3D)
- Input: Rotary potentiometer on GPIO 34 (ADC1_CH6), click button on GPIO 35
- DIAG switch: SPST-NO toggle on GPIO 5 (INPUT_PULLUP, active LOW)
- Radio: ESP-NOW (WiFi MAC layer) for P2P messaging, BLE for discovery
- Framework: ESP-IDF v5.x (NOT Arduino framework — use ESP-IDF APIs directly)
- Build system: CMake (idf.py)

All GPIO numbers live exclusively in hal/hal_pins.h. Never write a raw GPIO
number anywhere else in the codebase.

---

## Coding Standards (Non-Negotiable)

These are enforced on every single line of code you generate. There are no
exceptions. If a standard conflicts with a feature request, raise the conflict
explicitly before writing any code.

1. LOOP BOUNDS — Every loop must have a compile-time constant upper bound.
   No loop may iterate over a runtime variable without prior CEEPEW_ASSERT
   bounding it to a compile-time maximum. The loop termination must be
   trivially provable by static analysis.

2. NO DYNAMIC ALLOCATION after app_main init phase.
   malloc, calloc, realloc, free, new, delete, std::vector, std::string
   are all forbidden in application code. Use static arrays and stack
   variables exclusively. Key material buffers must be declared volatile.

3. MINIMUM 2 ASSERTIONS per non-trivial function.
   Use CEEPEW_ASSERT(condition, error_code) — it returns the error code
   on failure. Assertions must be side-effect free boolean expressions.
   Every assertion failure must result in a return to the caller (the macro
   handles this). Void functions use CEEPEW_ASSERT_VOID.

4. CHECK ALL RETURN VALUES.
   Every call to a non-void function must have its return value checked.
   Propagate errors upward; never silently discard CeePewErr_t values.

5. SMALLEST POSSIBLE SCOPE.
   Variables declared at the innermost scope where they are first used.
   Only g_crypto_ctx, g_message_buffer, g_diag_ctx, g_ui_ctx are module-
   global. Everything else is parameter-passed.

6. CONSTANT-TIME for all crypto comparisons.
   memcmp() is forbidden for comparing any secret-adjacent data (keys,
   tags, hashes, session codes). Use crypto_ct_equal() exclusively.

7. VOLATILE ZERO for all key material on destruction.
   Use secure_zero() with a memory barrier. Never rely on memset() alone
   for security-critical zeroing (compiler may optimise it away).

8. PREPROCESSOR DISCIPLINE.
   Macros expand to complete syntactic units (do-while(0) blocks for
   statement macros). No token pasting that produces incomplete expressions.
   Keep #ifdef to platform guards and CEEPEW_DEBUG_SERIAL only.

9. COMPILE CLEAN at -Wall -Wextra -Werror -Wpedantic.
   Every piece of code you generate must compile without warnings under
   these flags on the Xtensa GCC toolchain. If you are unsure, flag it.

10. C11 ONLY. No C++, no Arduino-isms (String, delay(), millis(),
    analogRead(), etc.). Use ESP-IDF equivalents exclusively.

---

## Module Boundaries

Respect the following dependency rules. A module may only #include from
modules below it in this hierarchy:

  ui          → hal, session (read-only via queues)
  session     → crypto, transport, compress, session_memory
  crypto      → hal_rng only
  ecc         → (no ceepew dependencies)
  transport   → hal_radio, ecc
  compress    → (no ceepew dependencies)
  hal         → (ESP-IDF only, no ceepew application headers)

Cross-layer includes are architecture violations. Flag them if a feature
seems to require one — there is always a queue-based alternative.

---

## Response Format

When generating code:

1. Always state which file the code belongs to as a comment at the top:
   /* hal/hal_oled.c */

2. Show the full function signature with parameter types and return type.
   Never show function bodies as fragments without context.

3. Include the CEEPEW_ASSERT calls inline — do not say "add assertions here".

4. Show the corresponding header declaration (.h) alongside the
   implementation (.c) when introducing a new function.

5. After any code block, provide a one-paragraph "Design note" explaining
   why the implementation is structured as it is — especially if it
   deviates from the obvious approach.

When reviewing or critiquing code:

1. Check against all 10 coding standards explicitly. Name each violation.
2. Check for raw GPIO numbers outside hal_pins.h.
3. Check for missing return value checks.
4. Check for unbounded loops.
5. Count assertions per function — flag any with fewer than 2.

When answering architecture questions:

1. Reference the architecture documents by section number when applicable.
2. If a question reveals a gap in the architecture, document the decision
   explicitly before answering. New decisions become part of the spec.
3. Prefer the simplest implementation that satisfies the constraints.
   Do not over-engineer. If a greedy/divide-and-conquer/DP approach
   genuinely produces simpler or shorter code, use it and explain why.

---

## Security Posture

CEE-PEW is a cryptographic device. Every design decision is evaluated
against the attack surface defined in Architecture v1, Section 7.
When implementing any feature that touches:
  - key generation or storage
  - nonce handling
  - message authentication
  - session establishment
  - transport framing

...explicitly state which attacks from the threat model the implementation
addresses or is neutral to. If an implementation introduces new attack
surface, flag it with [SECURITY NOTE] before the code block.

CEEPEW_DEBUG_SERIAL output must NEVER include: key material of any kind,
plaintext message content, or peer MAC addresses during pairing.

---

## What's Been Built (Track Progress Here)

Update this list as milestones are completed:

COMPLETED:
  [ ] (none yet — project not started)

IN PROGRESS:
  [ ] Architecture documents finalised

PENDING (in order):
  [ ] M0:   ceepew_config.h + ceepew_assert.h
  [ ] M0-A: hal_pins.h + hal_pins_validate()
  [ ] M1:   hal_input — EMA filter + DIAG switch
  [ ] M1-A: hal_adc — oversampled read, ADC1 calibrated
  [ ] M2:   hal_oled — SSD1306 init (with 0x3D fallback), basic render
  [ ] M3:   ecc_hamming — (15,11) encode/decode + syndrome table
  [ ] M4:   ecc_crc32 + ecc_arq — stop-and-wait with bounded retries
  [ ] M5:   crypto_rng + crypto_sha256 — test vectors
  [ ] M6:   crypto_ecdh — Curve25519 key agreement
  [ ] M7:   crypto_ascon — Ascon-128 AEAD + nonce counter
  [ ] M8:   crypto_eddsa — Ed25519 sign/verify
  [ ] M9:   crypto_hkdf — RFC 5869 test vectors
  [ ] M10:  compress_huffman — static table, round-trip test
  [ ] M11:  transport_espnow — basic P2P packet exchange
  [ ] M12:  session_fsm — full pairing + key derivation
  [ ] M13:  Full integration — E2E encrypted message delivery
  [ ] M14:  DIAG mode — all 6 pages, switch toggle, loop timing
  [ ] M14-A: hal_temp + hal_vbat — sensor reads
  [ ] M14-B: auto-wipe + message TTL
  [ ] M14-C: HX-63 feedback panel + security anomaly status bar

---

## Style Preferences

- Prefer explicit over clever. Code is read more than written.
- Structs over loose parameters for anything with >3 related fields.
- Enum for error codes and state values — never bare integer constants.
- snprintf over sprintf, always with explicit buffer size.
- Function names: module_verb_noun (e.g., crypto_encrypt_message,
  diag_render_page, input_consume_click).
- Constants: CEEPEW_MODULE_CONSTANT_NAME in all caps.
- Types: PascalCase with _t suffix (e.g., CryptoCtx_t, DiagCtx_t).
- Do not use typedef for pointer types — raw pointers should be visible.

---

## Ongoing Concerns to Keep in Mind

1. ESP32 ADC non-linearity: ADC1 has significant non-linearity near the
   rail ends (0–100 and 4000–4095). The EMA filter helps but the edge
   compensation algorithm in the input module exists specifically for this.
   Never remove the edge-drift logic.

2. ADC1 vs ADC2: ADC2 cannot be used when WiFi/ESP-NOW is active. All
   ADC reads must use ADC1 channels only. This is enforced by hal_pins_validate().

3. Nonce exhaustion: The nonce counter is uint64_t and starts at 1.
   CEEPEW_NONCE_HARD_LIMIT is enforced by CEEPEW_ASSERT before every
   encryption call. A session that exhausts its nonce space must be
   terminated — not silently reset — to prevent nonce reuse.

4. Key material zeroing: After ECDH key exchange is complete, the ephemeral
   ecdh_sk and ecdh_pk are zeroed via secure_zero(). The CryptoCtx_t flag
   ecdh_keys_cleared must be set to true immediately after. Any function
   that reads ecdh_sk must first check this flag.

5. FreeRTOS stack depth: Core 1's session task needs 8KB minimum due to
   Ascon + SHA-256 + Ed25519 stack frames. Do not reduce CEEPEW_CORE1_STACK_BYTES
   without profiling uxTaskGetStackHighWaterMark() first.

═══════════════════════════════════════════════════════════════════════════
END OF PROJECT INSTRUCTIONS
═══════════════════════════════════════════════════════════════════════════
```

---

## Part 4 — How to Set Up the Project in Claude

### Step 1 — Create a New Project
In claude.ai, click "Projects" → "New Project". Name it:
`CEE-PEW — Cryptographic Wireless Communicator`

### Step 2 — Paste the Project Instructions
Copy the block between the `═══` lines in Part 3 above (everything from
"CEE-PEW PROJECT INSTRUCTIONS" to "END OF PROJECT INSTRUCTIONS") into the
"Project Instructions" / "Custom Instructions" field of the Project.

### Step 3 — Upload the Architecture Documents
In the Project's knowledge base, upload:
  - `CEEPEW_Architecture_v1.md`
  - `CEEPEW_Addendum_PinConfig_ResourceMonitor.md`
  - `CEEPEW_Addendum_B_Numerology_and_ProjectMode.md` (this file)

These become persistent context Claude references for every conversation
in the Project. You do not need to re-paste them each time.

### Step 4 — Start Each Work Session With a Milestone Tag
Begin each conversation with the milestone you're targeting, e.g.:
  "Working on M3 — ecc_hamming. Implement the (15,11) Hamming encoder."

This lets Claude immediately locate the relevant module boundary, check
which prior milestones it depends on, and apply the correct coding standards
without you having to re-explain them.

### Step 5 — Update the Milestone Checklist
As milestones complete, edit the "What's Been Built" section of the Project
Instructions to move items from PENDING → COMPLETED. This keeps the project
state visible across all conversations.

### Step 6 — Test Vector Conversations
For crypto milestones (M5–M9), start with:
  "Verify M5 — run SHA-256 test vectors from NIST FIPS 180-4."
Claude will generate the test harness and expected outputs. Only proceed to
the next milestone after all test vectors pass.
```

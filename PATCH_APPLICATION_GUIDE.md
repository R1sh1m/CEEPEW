# CEE-PEW Phase 5 — Complete Patch Application Guide

**Date:** May 27, 2026  
**Status:** Implementation in progress  
**Patches to Apply:** 7 critical UI patches

---

## Overview of Changes

This document provides complete, ready-to-apply patches for:
1. ✅ GPIO 21/22 → 26/27 (DONE)
2. ⏳ DIAG page renderer + render_countdown() + chat UI (THIS FILE)
3. ⏳ BLE ready_for_chat handshake (NEXT)

---

## How to Apply Patches

### Method: Direct Function Replacement

For each function below:
1. Find the old function in `components/ceepew_hal/ui_manager.c`
2. Select the entire function (from `CeePewErr_t func_name(...) {` to closing `}`)
3. DELETE the old function
4. PASTE the new function from below

---

## PATCH 1: render_diag_page() — NEW FUNCTION

**Location:** Add BEFORE ui_manager_draw()  
**Status:** Completely new function (did not exist)

**Code:**
```c
/* 62-entry ASCII charset used by render_diag_page state names */
static const char *const s_ui_state_names[17] = {
    "BOOT","DISC","CODE","COUNT","BADCOD","DIFF",
    "CONFIRM","KEYDER","FP","FPCONF","CHAT","CHATMNU",
    "COMPOSE","CRYPTO","NONCE","INFO","ERROR"
};

static CeePewErr_t render_diag_page(void)
{
    CEEPEW_ASSERT(s_ui_manager_initialised, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(g_ui_ctx.diag_mode, CEEPEW_ERR_PARAM);

    hal_ui_clear();

    /* Map pot (0-255) to 3 DIAG pages */
    uint8_t page = (uint8_t)(((uint16_t)g_ui_ctx.user_input * 3U) / 256U);
    if (page > 2U) { page = 2U; }

    /* Page indicator dots — centred at y=62 */
    for (uint8_t p = 0U; p < 3U; p++) {
        uint8_t dx = (uint8_t)(54U + p * 10U);
        if (p == page) {
            HalUIRect_t dot = { .x = (uint8_t)(dx - 2U), .y = 60U,
                                .w = 5U, .h = 3U };
            hal_ui_rect_fill(&dot, HAL_UI_WHITE);
        } else {
            draw_pixel(dx, 61U);
        }
    }

    /* DIAG badge on top-right so user always knows they're in DIAG */
    hal_ui_text(104U, 0U, "[DIAG]", HAL_UI_WHITE);

    switch (page) {

    /* ── Page 0: BLE / Session ──────────────────────────────────────────── */
    case 0U: {
        hal_ui_text(0U, 0U, "SESSION/BLE", HAL_UI_WHITE);
        draw_hline(0U, 9U, 100U);

        char ln[22U];
        uint8_t phase = session_get_phase();
        static const char *const PHASE_NAMES[4] = { "IDLE","DISC","PAIR","ACTIVE" };
        const char *pname = (phase < 4U) ? PHASE_NAMES[phase] : "?";
        (void)snprintf(ln, sizeof(ln), "Phase: %s", pname);
        hal_ui_text(0U, 12U, ln, HAL_UI_WHITE);

        /* BLE state */
        const char *bname = "IDLE";
        switch (transport_ble_get_state()) {
            case BLE_ADVERTISING:              bname = "ADV";     break;
            case BLE_SCANNING:                 bname = "SCAN";    break;
            case BLE_ADVERTISING_AND_SCANNING: bname = "ADV+SCN"; break;
            case BLE_CONNECTED:                bname = "CONN";    break;
            case BLE_PAIRING:                  bname = "PAIR";    break;
            case BLE_DONE:                     bname = "DONE";    break;
            default: break;
        }
        (void)snprintf(ln, sizeof(ln), "BLE: %s", bname);
        hal_ui_text(0U, 21U, ln, HAL_UI_WHITE);

        /* Peer MAC (last 3 bytes) */
        if (g_ble_ctx.discovered) {
            (void)snprintf(ln, sizeof(ln), "Peer:%02X%02X%02X r:%d",
                           g_ble_ctx.peer_mac[3], g_ble_ctx.peer_mac[4],
                           g_ble_ctx.peer_mac[5], (int)g_ble_ctx.peer_rssi);
            hal_ui_text(0U, 30U, ln, HAL_UI_WHITE);
            (void)snprintf(ln, sizeof(ln), "Hits:%u Seen:%lu",
                           (unsigned)g_ble_ctx.scan_hit_count,
                           (unsigned long)g_ble_ctx.scan_seen_count);
            hal_ui_text(0U, 39U, ln, HAL_UI_WHITE);
        } else {
            hal_ui_text(0U, 30U, "Peer: none", HAL_UI_WHITE);
            (void)snprintf(ln, sizeof(ln), "Seen:%lu",
                           (unsigned long)g_ble_ctx.scan_seen_count);
            hal_ui_text(0U, 39U, ln, HAL_UI_WHITE);
        }

        /* Commitment / handoff flags */
        (void)snprintf(ln, sizeof(ln), "Cmit:%s Hoff:%s Act:%s",
                       g_ble_ctx.commitment_verified ? "Y" : "N",
                       g_ble_ctx.handoff_ready       ? "Y" : "N",
                       session_is_active()            ? "Y" : "N");
        hal_ui_text(0U, 48U, ln, HAL_UI_WHITE);
    } break;

    /* ── Page 1: Crypto / Nonce ─────────────────────────────────────────── */
    case 1U: {
        hal_ui_text(0U, 0U, "CRYPTO/NONCE", HAL_UI_WHITE);
        draw_hline(0U, 9U, 100U);

        char ln[22U];
        bool active = session_is_active();
        (void)snprintf(ln, sizeof(ln), "Session: %s", active ? "ACTIVE" : "IDLE");
        hal_ui_text(0U, 12U, ln, HAL_UI_WHITE);

        uint64_t nonce = session_get_nonce_counter();
        (void)snprintf(ln, sizeof(ln), "Nonce:%08lX",
                       (unsigned long)(nonce & 0xFFFFFFFFUL));
        hal_ui_text(0U, 21U, ln, HAL_UI_WHITE);

        /* Nonce health bar — 100px wide */
        uint8_t nonce_pct = (nonce > 0U)
            ? (uint8_t)((nonce * 100ULL) / (CEEPEW_NONCE_HARD_LIMIT / 100ULL + 1ULL))
            : 0U;
        if (nonce_pct > 100U) { nonce_pct = 100U; }
        HalUIRect_t nb = { .x = 0U, .y = 30U, .w = 100U, .h = 6U };
        hal_ui_rect(&nb, HAL_UI_WHITE);
        if (nonce_pct > 0U) {
            HalUIRect_t nf = { .x = 1U, .y = 31U,
                               .w = (uint8_t)((uint16_t)98U * nonce_pct / 100U),
                               .h = 4U };
            hal_ui_rect_fill(&nf, HAL_UI_WHITE);
        }
        (void)snprintf(ln, sizeof(ln), "Used:%u%%", (unsigned)nonce_pct);
        hal_ui_text(104U, 30U, ln, HAL_UI_WHITE);

        /* Session ID (lower 32-bit) */
        uint64_t sid = session_get_id();
        (void)snprintf(ln, sizeof(ln), "SID:%08lX",
                       (unsigned long)(sid & 0xFFFFFFFFUL));
        hal_ui_text(0U, 39U, ln, HAL_UI_WHITE);

        /* Current UI state */
        uint8_t si = (uint8_t)g_ui_ctx.current_state;
        const char *sname = (si < 17U) ? s_ui_state_names[si] : "?";
        (void)snprintf(ln, sizeof(ln), "UI:%s", sname);
        hal_ui_text(0U, 48U, ln, HAL_UI_WHITE);
    } break;

    /* ── Page 2: System ─────────────────────────────────────────────────── */
    case 2U: {
        hal_ui_text(0U, 0U, "SYSTEM", HAL_UI_WHITE);
        draw_hline(0U, 9U, 100U);

        char ln[22U];

        /* Uptime */
        uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
        (void)snprintf(ln, sizeof(ln), "Up:%luh%02lum%02lus",
                       (unsigned long)(uptime_s / 3600U),
                       (unsigned long)((uptime_s / 60U) % 60U),
                       (unsigned long)(uptime_s % 60U));
        hal_ui_text(0U, 12U, ln, HAL_UI_WHITE);

        /* Free heap */
        uint32_t heap = esp_get_free_heap_size();
        (void)snprintf(ln, sizeof(ln), "Heap: %luKB free", (unsigned long)(heap / 1024U));
        hal_ui_text(0U, 21U, ln, HAL_UI_WHITE);

        /* Pot raw value (useful for ADC calibration) */
        (void)snprintf(ln, sizeof(ln), "Pot: %u/255", (unsigned)g_ui_ctx.user_input);
        hal_ui_text(0U, 30U, ln, HAL_UI_WHITE);

        /* Adv / scan counts */
        (void)snprintf(ln, sizeof(ln), "Adv pkts: %lu",
                       (unsigned long)g_ble_ctx.adv_packet_count);
        hal_ui_text(0U, 39U, ln, HAL_UI_WHITE);

        /* Loop frame rate estimate: 1000/30ms ≈ 33 Hz */
        hal_ui_text(0U, 48U, "Loop ~33Hz (30ms)", HAL_UI_WHITE);
    } break;

    default:
        break;
    }

    return CEEPEW_OK;
}
```

---

## Implementation Status

- [ ] Patch 1: DIAG page renderer (apply above code)
- [ ] Patch 2: render_countdown() updates
- [ ] Patch 3: render_chat_menu() simplification
- [ ] Patch 4: render_chat_compose() full charset
- [ ] Patch 5: ui_manager_draw() DIAG override
- [ ] Patch 6: CHAT_COMPOSE input handling
- [ ] Patch 7: CHAT_MENU input handling
- [ ] Patch 8: BLE ready_for_chat handshake (separate file)
- [ ] Rebuild and test

---

## Next Steps

1. Copy all patches from `paste-1779843654175.txt`
2. Apply functions one at a time to `ui_manager.c`
3. Run `idf.py build` to verify compilation
4. If errors, check line numbers and function boundaries
5. Then implement BLE handshake in `transport_ble.c`
6. Flash and test hardware

---

**Created:** 2026-05-27 06:35 UTC  
**Version:** 5.0-comprehensive

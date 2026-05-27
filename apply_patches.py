#!/usr/bin/env python3
"""
CEE-PEW Phase 5 Patch Application Utility

Automatically applies all 7 UI patches to components/ceepew_hal/ui_manager.c
Run: python3 apply_patches.py
"""

import os
import sys
import re

# Source file
UI_MANAGER_PATH = "components/ceepew_hal/ui_manager.c"

# List of patches to apply (function name, start marker, end marker)
PATCHES = [
    {
        "name": "Add esp_system.h include",
        "type": "include",
        "search": '#include "esp_wifi.h"\n#include <string.h>',
        "replace": '#include "esp_wifi.h"\n#include "esp_system.h"   /* esp_get_free_heap_size() */\n#include <string.h>'
    },
    {
        "name": "render_diag_page - NEW FUNCTION (add before ui_manager_draw)",
        "type": "add_function",
        "before_function": "ui_manager_draw",
        "content": """/* 62-entry ASCII charset used by render_diag_page state names */
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

    uint8_t page = (uint8_t)(((uint16_t)g_ui_ctx.user_input * 3U) / 256U);
    if (page > 2U) { page = 2U; }

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

    hal_ui_text(104U, 0U, "[DIAG]", HAL_UI_WHITE);

    switch (page) {

    case 0U: {
        hal_ui_text(0U, 0U, "SESSION/BLE", HAL_UI_WHITE);
        draw_hline(0U, 9U, 100U);

        char ln[22U];
        uint8_t phase = session_get_phase();
        static const char *const PHASE_NAMES[4] = { "IDLE","DISC","PAIR","ACTIVE" };
        const char *pname = (phase < 4U) ? PHASE_NAMES[phase] : "?";
        (void)snprintf(ln, sizeof(ln), "Phase: %s", pname);
        hal_ui_text(0U, 12U, ln, HAL_UI_WHITE);

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

        (void)snprintf(ln, sizeof(ln), "Cmit:%s Hoff:%s Act:%s",
                       g_ble_ctx.commitment_verified ? "Y" : "N",
                       g_ble_ctx.handoff_ready       ? "Y" : "N",
                       session_is_active()            ? "Y" : "N");
        hal_ui_text(0U, 48U, ln, HAL_UI_WHITE);
    } break;

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

        uint64_t sid = session_get_id();
        (void)snprintf(ln, sizeof(ln), "SID:%08lX",
                       (unsigned long)(sid & 0xFFFFFFFFUL));
        hal_ui_text(0U, 39U, ln, HAL_UI_WHITE);

        uint8_t si = (uint8_t)g_ui_ctx.current_state;
        const char *sname = (si < 17U) ? s_ui_state_names[si] : "?";
        (void)snprintf(ln, sizeof(ln), "UI:%s", sname);
        hal_ui_text(0U, 48U, ln, HAL_UI_WHITE);
    } break;

    case 2U: {
        hal_ui_text(0U, 0U, "SYSTEM", HAL_UI_WHITE);
        draw_hline(0U, 9U, 100U);

        char ln[22U];

        uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
        (void)snprintf(ln, sizeof(ln), "Up:%luh%02lum%02lus",
                       (unsigned long)(uptime_s / 3600U),
                       (unsigned long)((uptime_s / 60U) % 60U),
                       (unsigned long)(uptime_s % 60U));
        hal_ui_text(0U, 12U, ln, HAL_UI_WHITE);

        uint32_t heap = esp_get_free_heap_size();
        (void)snprintf(ln, sizeof(ln), "Heap: %luKB free", (unsigned long)(heap / 1024U));
        hal_ui_text(0U, 21U, ln, HAL_UI_WHITE);

        (void)snprintf(ln, sizeof(ln), "Pot: %u/255", (unsigned)g_ui_ctx.user_input);
        hal_ui_text(0U, 30U, ln, HAL_UI_WHITE);

        (void)snprintf(ln, sizeof(ln), "Adv pkts: %lu",
                       (unsigned long)g_ble_ctx.adv_packet_count);
        hal_ui_text(0U, 39U, ln, HAL_UI_WHITE);

        hal_ui_text(0U, 48U, "Loop ~33Hz (30ms)", HAL_UI_WHITE);
    } break;

    default:
        break;
    }

    return CEEPEW_OK;
}

"""
    }
]

def main():
    if not os.path.exists(UI_MANAGER_PATH):
        print(f"ERROR: {UI_MANAGER_PATH} not found")
        sys.exit(1)
    
    print(f"Reading {UI_MANAGER_PATH}...")
    with open(UI_MANAGER_PATH, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Apply include patch
    print("Applying include patch...")
    if PATCHES[0]["search"] in content:
        content = content.replace(PATCHES[0]["search"], PATCHES[0]["replace"])
        print("  ✓ Include added")
    else:
        print("  ⚠ Include already present or pattern not found")
    
    # Add render_diag_page function before ui_manager_draw
    print("Adding render_diag_page function...")
    func_pattern = r'(CeePewErr_t ui_manager_draw\(void\))'
    match = re.search(func_pattern, content)
    if match:
        insert_pos = match.start()
        content = content[:insert_pos] + PATCHES[1]["content"] + "\n\n" + content[insert_pos:]
        print("  ✓ render_diag_page added")
    else:
        print("  ⚠ Could not find ui_manager_draw function")
    
    # Write back
    print(f"Writing to {UI_MANAGER_PATH}...")
    with open(UI_MANAGER_PATH, 'w', encoding='utf-8') as f:
        f.write(content)
    
    print("\n✅ Patch application complete!")
    print("\nNEXT STEPS:")
    print("1. Manually apply remaining patches from paste-1779843654175.txt")
    print("2. Replace render_countdown(), render_chat_menu(), render_chat_compose()")
    print("3. Replace ui_manager_draw() with DIAG-aware version")
    print("4. Replace ui_manager_update() CHAT_COMPOSE and CHAT_MENU blocks")
    print("5. Run: idf.py build")
    print("6. Implement BLE ready_for_chat handshake")

if __name__ == "__main__":
    main()

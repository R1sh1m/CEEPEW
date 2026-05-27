#!/bin/bash
# Apply all UI patches from paste-1779843654175.txt to ui_manager.c
# This is a manual step-by-step guide for maximum safety

cd "$(dirname "$0")" || exit 1

FILE="components/ceepew_hal/ui_manager.c"

if [ ! -f "$FILE" ]; then
    echo "ERROR: $FILE not found"
    exit 1
fi

echo "Applying CEE-PEW Phase 5 UI Patches..."
echo ""

# STEP 1: Ensure esp_system.h is included (needed for esp_get_free_heap_size)
echo "Step 1: Adding esp_system.h include..."
if ! grep -q '#include "esp_system.h"' "$FILE"; then
    sed -i '/#include "esp_wifi.h"/a #include "esp_system.h"   /* esp_get_free_heap_size() */' "$FILE"
    echo "  ✓ Added"
else
    echo "  ✓ Already present"
fi

echo ""
echo "Patches added. Remaining patches require manual application:"
echo "  - render_diag_page() - add before ui_manager_draw()"
echo "  - render_countdown() - replace function"
echo "  - render_chat_menu() - replace function"
echo "  - render_chat_compose() - replace function"
echo "  - ui_manager_draw() - replace function (add DIAG override)"
echo "  - ui_manager_update() CHAT_COMPOSE block - replace"
echo "  - ui_manager_update() CHAT_MENU block - replace"
echo ""
echo "Source: C:\Users\Rishi Misra\.copilot\session-state\8f39fca0-7081-4a04-a408-a3fd0e05755b\files\paste-1779843654175.txt"
echo ""
echo "Next: Run 'idf.py build' to test incremental application"

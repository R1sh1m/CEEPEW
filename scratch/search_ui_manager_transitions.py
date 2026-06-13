import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("components/ceepew_hal/ui_manager.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

print("=== UI_STATE_CODE_ENTRY occurrences in ui_manager.c ===")
for idx, line in enumerate(lines, 1):
    if "UI_STATE_CODE_ENTRY" in line:
        print(f"{idx}: {line.strip()}")

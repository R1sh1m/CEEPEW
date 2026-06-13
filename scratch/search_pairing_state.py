import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("components/ceepew_hal/ui_manager.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

print("=== UI_STATE_PAIRING occurrences in ui_manager.c ===")
for idx, line in enumerate(lines, 1):
    if "UI_STATE_PAIRING" in line:
        # Avoid print lines that are just case labels unless they do something
        if any(w in line for w in ["transition", "current_state", "next_state", "==", "=", "if"]):
            print(f"{idx}: {line.strip()}")

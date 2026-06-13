import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("components/transport/transport_ble.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

# Search for gattc_profile_event_handler or similar GATTC handlers
start_line = -1
for idx, line in enumerate(lines, 1):
    if "gattc_profile_event_handler" in line or "esp_gattc_cb" in line or "gattc_cb" in line:
        if "static" in line or "void" in line:
            start_line = idx
            break

if start_line != -1:
    print(f"Found GATTC event handler at line {start_line}")
    for i in range(start_line - 1, min(len(lines), start_line + 250)):
        print(f"{i+1}: {lines[i]}")
else:
    print("GATTC event handler not found")

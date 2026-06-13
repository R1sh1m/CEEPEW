import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("components/transport/transport_ble.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

# Search for ESP_GATTS_WRITE_EVT
start_line = -1
for idx, line in enumerate(lines, 1):
    if "ESP_GATTS_WRITE_EVT" in line:
        start_line = idx
        break

if start_line != -1:
    print(f"Found ESP_GATTS_WRITE_EVT at line {start_line}")
    for i in range(start_line - 5, min(len(lines), start_line + 120)):
        print(f"{i+1}: {lines[i]}")
else:
    print("ESP_GATTS_WRITE_EVT not found")

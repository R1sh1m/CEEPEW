import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("components/transport/transport_ble.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

# Search for transport_ble_scan_peer_is_duplicate
start_line = -1
for idx, line in enumerate(lines, 1):
    if "transport_ble_scan_peer_is_duplicate" in line:
        if "static" in line or "bool" in line:
            start_line = idx
            break

if start_line != -1:
    print(f"Found function at line {start_line}")
    for i in range(start_line - 1, min(len(lines), start_line + 40)):
        print(f"{i+1}: {lines[i]}")
else:
    print("Function not found")

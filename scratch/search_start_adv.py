import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("components/transport/transport_ble.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

start_line = -1
for idx, line in enumerate(lines, 1):
    if "CeePewErr_t transport_ble_start_advertising(" in line:
        start_line = idx
        break

if start_line != -1:
    print(f"Found function definition at line {start_line}")
    for i in range(start_line - 1, min(len(lines), start_line + 45)):
        print(f"{i+1}: {lines[i]}")
else:
    print("Function definition not found")

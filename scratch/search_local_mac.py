import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("components/transport/transport_ble.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

print("=== local_mac occurrences in transport_ble.c ===")
for idx, line in enumerate(lines, 1):
    if "local_mac" in line:
        print(f"{idx}: {line.strip()}")

import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("components/transport/transport_ble.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

print("=== stop_advertising occurrences ===")
for idx, line in enumerate(lines, 1):
    if "stop_advertising" in line.lower() or "gap_stop_adv" in line.lower():
        print(f"{idx}: {line.strip()}")

import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("components/transport/transport_ble.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

print("=== ADV_STOP_COMPLETE occurrences ===")
for idx, line in enumerate(lines, 1):
    if "ADV_STOP_COMPLETE" in line:
        print(f"{idx}: {line.strip()}")

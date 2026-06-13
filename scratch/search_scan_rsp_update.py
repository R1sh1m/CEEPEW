import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("components/transport/transport_ble.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

print("=== s_scan_rsp_update_only occurrences ===")
for idx, line in enumerate(lines, 1):
    if "s_scan_rsp_update_only" in line:
        print(f"{idx}: {line.strip()}")

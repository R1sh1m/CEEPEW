import re

with open("components/transport/transport_ble.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

# Search for gatt_ready, open, connect, and print matching lines with context
keywords = ["gatt_ready", "brief gatt", "open", "connect", "commitment_beacon", "verify_commitment"]
for idx, line in enumerate(lines, 1):
    found = [kw for kw in keywords if kw in line.lower()]
    if found:
        # Check if it's a function definition or a log message or state transition
        if any(w in line for w in ["ESP_LOG", "phase", "state", "==", "true", "false", "=", "if", "else", "void", "static"]):
            print(f"{idx}: {line.strip()}")

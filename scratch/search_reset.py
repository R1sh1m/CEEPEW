with open("components/transport/transport_ble.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

print("=== reset function definitions/calls ===")
for idx, line in enumerate(lines, 1):
    if "reset" in line.lower() and ("static" in line or "void" in line or "CeePewErr" in line):
        print(f"{idx}: {line.strip()}")

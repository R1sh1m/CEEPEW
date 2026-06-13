with open("components/transport/transport_ble.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

print("=== beacon_nonce_peer_counter_max occurrences ===")
for idx, line in enumerate(lines, 1):
    if "beacon_nonce_peer_counter_max" in line:
        print(f"{idx}: {line.strip()}")

print("\n=== beacon_nonce_local occurrences ===")
for idx, line in enumerate(lines, 1):
    if "beacon_nonce_local" in line:
        print(f"{idx}: {line.strip()}")

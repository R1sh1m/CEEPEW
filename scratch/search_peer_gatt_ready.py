import os

for root, dirs, files in os.walk("."):
    for file in files:
        if file.endswith((".c", ".h")):
            filepath = os.path.join(root, file)
            with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
                content = f.read()
            if "peer_gatt_ready" in content:
                print(f"=== {filepath} ===")
                for line_no, line in enumerate(content.splitlines(), 1):
                    if "peer_gatt_ready" in line:
                        print(f"  {line_no}: {line.strip()}")

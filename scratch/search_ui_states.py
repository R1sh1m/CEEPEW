import os

for root, dirs, files in os.walk("."):
    for file in files:
        if file.endswith((".c", ".h")):
            filepath = os.path.join(root, file)
            with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
                content = f.read()
            if "UI_STATE_" in content:
                print(f"=== {filepath} ===")
                for line_no, line in enumerate(content.splitlines(), 1):
                    if "UI_STATE_" in line and ("enum" in line or "#define" in line or "case" in line or "=" in line):
                        print(f"  {line_no}: {line.strip()}")

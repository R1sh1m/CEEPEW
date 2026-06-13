import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("main/task_session.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

# Search for task function signature or while loops
for idx, line in enumerate(lines, 1):
    if "void" in line and "(" in line and "pvParameters" in line:
        print(f"Task definition: {idx}: {line.strip()}")
    elif "while" in line and ("1" in line or "true" in line):
        print(f"Loop: {idx}: {line.strip()}")

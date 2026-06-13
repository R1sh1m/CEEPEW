import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("main/task_session.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

print("=== sync_visual_state occurrences ===")
for idx, line in enumerate(lines, 1):
    if "sync_visual_state" in line:
        print(f"{idx}: {line.strip()}")

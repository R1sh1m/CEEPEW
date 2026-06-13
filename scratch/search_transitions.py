import sys

sys.stdout.reconfigure(encoding='utf-8')

with open("main/task_session.c", "r", encoding="utf-8") as f:
    content = f.read()

lines = content.splitlines()

print("=== ui_manager_transition_to occurrences in task_session.c ===")
for idx, line in enumerate(lines, 1):
    if "ui_manager_transition_to" in line:
        print(f"{idx}: {line.strip()}")

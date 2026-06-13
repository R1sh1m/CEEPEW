import re

with open("main/session_fsm.c", "r", encoding="utf-8") as f:
    content = f.read()

# Let's find state transition functions or phase definitions
print("=== Phase list or state transition occurrences ===")
for line_no, line in enumerate(content.splitlines(), 1):
    if "transition" in line.lower() or "phase" in line.lower() or "state" in line.lower():
        if "session_fsm" in line.lower() or "phase" in line or "state" in line:
            if re.search(r"\b(static|void|int|session_state_t|pairing_phase_t)\b", line):
                print(f"{line_no}: {line}")

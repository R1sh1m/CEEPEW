import os

tasks_dir = r"C:\Users\Rishi Misra\.gemini\antigravity-cli\brain\fc01803b-e02f-4909-b5e7-fce24d3912d3\.system_generated\tasks"

def print_log_after_matching(filename, line_no):
    filepath = os.path.join(tasks_dir, filename)
    if not os.path.exists(filepath):
        print(f"File not found: {filename}")
        return
    print(f"\n==================== {filename} after line {line_no} ====================")
    with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
        lines = f.readlines()
    
    # Print 100 lines after line_no
    start = max(0, line_no - 5)
    end = min(len(lines), line_no + 120)
    for idx in range(start, end):
        print(f"{idx}: {lines[idx].strip()}")

print_log_after_matching("task-2679.log", 3280)
print_log_after_matching("task-2577.log", 6870)

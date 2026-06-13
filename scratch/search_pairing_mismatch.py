import os
import sys

sys.stdout.reconfigure(encoding='utf-8')

tasks_dir = r"C:\Users\Rishi Misra\.gemini\antigravity-cli\brain\fc01803b-e02f-4909-b5e7-fce24d3912d3\.system_generated\tasks"
target_files = ["task-2577.log", "task-2679.log", "task-2717.log"]

for filename in target_files:
    filepath = os.path.join(tasks_dir, filename)
    if not os.path.exists(filepath):
        print(f"File {filename} does not exist.")
        continue
    print(f"\n==================== Matches in {filename} ====================")
    with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
        lines = f.readlines()
    
    for idx, line in enumerate(lines):
        if "gap_event_handler" in line:
            continue
        if any(kw in line for kw in ["verification failed", "verification PASSED", "verify_pending", "mismatch", "failed", "verr=", "gatt_ready", "GATTC open", "GATTS connected", "GATTC connected"]):
            # exclude unit test lines
            if "TEST-" in line or "selftest" in line:
                continue
            print(f"{idx}: {line.strip()}")

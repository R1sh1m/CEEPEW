import os
import sys

sys.stdout.reconfigure(encoding='utf-8')

tasks_dir = r"C:\Users\Rishi Misra\.gemini\antigravity-cli\brain\fc01803b-e02f-4909-b5e7-fce24d3912d3\.system_generated\tasks"
log_files = [f for f in os.listdir(tasks_dir) if f.endswith(".log") and os.path.getsize(os.path.join(tasks_dir, f)) > 100000]

# Sort by modification time to see newest first
log_files.sort(key=lambda x: os.path.getmtime(os.path.join(tasks_dir, x)), reverse=True)

print(f"Scanning {len(log_files)} large log files for real device pairing events...")

for filename in log_files:
    filepath = os.path.join(tasks_dir, filename)
    with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
        lines = f.readlines()
    
    # We want to find lines containing "[DEVICE_" and any key pairing event: "verification failed", "commitment", "failed", "mismatch", "mismatch", etc.
    found = False
    for idx, line in enumerate(lines):
        if "[DEVICE_" in line and any(kw in line.lower() for kw in ["verification failed", "mismatch", "failed", "verr="]):
            if "selftest" in line.lower() or "test-" in line.lower():
                continue
            print(f"\n==================== {filename} Line {idx} ====================")
            start = max(0, idx - 15)
            end = min(len(lines), idx + 25)
            for j in range(start, end):
                print(f"{j}: {lines[j].strip()}")
            found = True
            break # just print first match per file

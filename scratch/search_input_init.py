import sys

log_file_path = r"C:\Users\Rishi Misra\.gemini\antigravity-cli\brain\fc01803b-e02f-4909-b5e7-fce24d3912d3\.system_generated\tasks\task-1643.log"

sys.stdout.reconfigure(encoding='utf-8')

with open(log_file_path, 'rb') as f:
    for line in f:
        if b"input_init" in line or b"input_update" in line or b"BUTTON_PRESSED" in line:
            print(line.decode('utf-8', errors='replace').strip())

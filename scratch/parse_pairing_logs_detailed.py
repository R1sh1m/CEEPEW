import sys

log_file_path = r"C:\Users\Rishi Misra\.gemini\antigravity-cli\brain\fc01803b-e02f-4909-b5e7-fce24d3912d3\.system_generated\tasks\task-1406.log"

sys.stdout.reconfigure(encoding='utf-8')

with open(log_file_path, 'rb') as f:
    for line in f:
        parts = line.split(b' ')
        if len(parts) >= 3 and parts[2].startswith(b'(') and parts[2].endswith(b')'):
            try:
                uptime_ms = int(parts[2][1:-1])
                # Pairing state entry happens around 90-100 seconds
                if 90000 <= uptime_ms <= 100000:
                    # Ignore generic scan signals to keep it clean
                    if b"Scan signal" in line and b"CEEPEW" not in line and b"ceepew" not in line:
                        continue
                    print(line.decode('utf-8', errors='replace').strip())
            except ValueError:
                pass

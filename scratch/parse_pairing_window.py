import sys

log_file_path = r"C:\Users\Rishi Misra\.gemini\antigravity-cli\brain\fc01803b-e02f-4909-b5e7-fce24d3912d3\.system_generated\tasks\task-1406.log"

sys.stdout.reconfigure(encoding='utf-8')

with open(log_file_path, 'rb') as f:
    for line in f:
        # Get uptime in ms
        # Format of line: [DEVICE_A_COM5] I (140700) ...
        # Let's extract the number inside the parentheses
        parts = line.split(b' ')
        if len(parts) >= 3 and parts[2].startswith(b'(') and parts[2].endswith(b')'):
            try:
                uptime_ms = int(parts[2][1:-1])
                if 90000 <= uptime_ms <= 142000:
                    # Ignore spammy scan signals unless ceepew
                    if b"Scan signal" in line and b"CEEPEW" not in line and b"ceepew" not in line:
                        continue
                    print(line.decode('utf-8', errors='replace').strip())
            except ValueError:
                pass

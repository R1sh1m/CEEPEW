import sys

log_file_path = r"C:\Users\Rishi Misra\.gemini\antigravity-cli\brain\fc01803b-e02f-4909-b5e7-fce24d3912d3\.system_generated\tasks\task-1406.log"

sys.stdout.reconfigure(encoding='utf-8')

with open(log_file_path, 'rb') as f:
    last_phase_a = None
    last_phase_b = None
    for line in f:
        # Check if line contains phase=
        if b"phase=" in line:
            parts = line.split(b' ')
            if len(parts) >= 3 and parts[2].startswith(b'(') and parts[2].endswith(b')'):
                try:
                    uptime_ms = int(parts[2][1:-1])
                    if uptime_ms < 30000:
                        continue # Skip startup tests
                    
                    # Extract phase value
                    # phase=... is inside line
                    for word in line.split():
                        if word.startswith(b"phase="):
                            phase = word.split(b"=")[1]
                            if b"DEVICE_A_COM5" in line:
                                if phase != last_phase_a:
                                    print(f"[A] {uptime_ms} ms: phase={phase.decode()} (from {last_phase_a})")
                                    last_phase_a = phase
                            elif b"DEVICE_B_COM6" in line:
                                if phase != last_phase_b:
                                    print(f"[B] {uptime_ms} ms: phase={phase.decode()} (from {last_phase_b})")
                                    last_phase_b = phase
                except Exception:
                    pass

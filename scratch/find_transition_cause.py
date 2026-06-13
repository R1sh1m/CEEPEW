import sys

log_file_path = r"C:\Users\Rishi Misra\.gemini\antigravity-cli\brain\fc01803b-e02f-4909-b5e7-fce24d3912d3\.system_generated\tasks\task-1406.log"

sys.stdout.reconfigure(encoding='utf-8')

with open(log_file_path, 'rb') as f:
    lines = f.readlines()

def print_context(device_name):
    # Find first index where ui=pair_fail occurs
    idx = -1
    for i, line in enumerate(lines):
        if device_name.encode() in line and b"ui=pair_fail" in line:
            idx = i
            break
    
    if idx != -1:
        print(f"=== First occurrence of ui=pair_fail for {device_name} (line {idx}) ===")
        # Print 15 lines before
        start = max(0, idx - 15)
        for i in range(start, idx + 5):
            if i < len(lines):
                print(lines[i].decode('utf-8', errors='replace').strip())
    else:
        print(f"=== ui=pair_fail not found for {device_name} ===")

print_context("DEVICE_A_COM5")
print_context("DEVICE_B_COM6")

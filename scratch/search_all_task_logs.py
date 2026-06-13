import os

tasks_dir = r"C:\Users\Rishi Misra\.gemini\antigravity-cli\brain\fc01803b-e02f-4909-b5e7-fce24d3912d3\.system_generated\tasks"
search_terms = [
    "Commitment beacon queued",
    "GATTC open",
    "GATTC connected",
    "GATTS connected",
    "GATTC open failed",
    "esp_ble_gattc_open",
    "open failed",
    "brief GATT",
    "Beacon match",
    "verify_commitment",
    "transition",
    "PHASE_TIMEOUT",
    "PAIRING FAILED",
    "PAIRING LINK FAILED"
]

print("Scanning task logs...")
for filename in os.listdir(tasks_dir):
    if not filename.endswith(".log"):
        continue
    filepath = os.path.join(tasks_dir, filename)
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            content = f.read()
        
        matches = []
        for term in search_terms:
            if term in content:
                # Count occurrences
                count = content.count(term)
                matches.append(f"{term} ({count})")
        
        if matches:
            print(f"{filename} (size {os.path.getsize(filepath)} bytes):")
            print("  " + ", ".join(matches))
            # If the log is large and has many matches, let's print the last few lines containing any match
            lines = content.splitlines()
            printed_lines = 0
            for idx, line in enumerate(lines):
                if any(term in line for term in search_terms):
                    # print line with line index
                    # limit to printing max 10 lines per file to avoid too much output
                    if printed_lines < 10:
                        print(f"    Line {idx}: {line.strip()}")
                        printed_lines += 1
            if printed_lines >= 10:
                print("    ...")
    except Exception as e:
        print(f"Error reading {filename}: {e}")

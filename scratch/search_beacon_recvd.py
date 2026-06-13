import os

tasks_dir = r"C:\Users\Rishi Misra\.gemini\antigravity-cli\brain\fc01803b-e02f-4909-b5e7-fce24d3912d3\.system_generated\tasks"
search_term = "Commitment beacon received"

print(f"Searching for '{search_term}'...")
for filename in os.listdir(tasks_dir):
    if not filename.endswith(".log"):
        continue
    filepath = os.path.join(tasks_dir, filename)
    with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
        content = f.read()
    if search_term in content:
        print(f"Found in {filename} ({content.count(search_term)} times)")
        # Print lines containing the term
        for line in content.splitlines():
            if search_term in line:
                print(f"  {line.strip()}")

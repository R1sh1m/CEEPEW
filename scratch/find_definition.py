import os

search_dir = r"C:\Users\Rishi Misra\Desktop\Code\ESP32\ceepew"
query = "hal_pins.h"

for root, dirs, files in os.walk(search_dir):
    for f in files:
        if f.endswith(('.c', '.h', '.cpp', '.py', '.txt')):
            path = os.path.join(root, f)
            try:
                with open(path, 'r', encoding='utf-8', errors='ignore') as file:
                    for i, line in enumerate(file, 1):
                        if query in line:
                            print(f"Found in {path}:{i} -> {line.strip()}")
            except Exception:
                pass

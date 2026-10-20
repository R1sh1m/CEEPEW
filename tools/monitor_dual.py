import serial, threading, time, sys, os

LOGS_DIR = "logs"
os.makedirs(LOGS_DIR, exist_ok=True)
ts = time.strftime("%Y%m%d_%H%M%S")
log_path = os.path.join(LOGS_DIR, f"pairing_bench_{ts}.txt")

# Parse from command line or use defaults
import argparse
parser = argparse.ArgumentParser()
parser.add_argument("--ports", default="COM3,COM4")
parser.add_argument("--baud", type=int, default=921600)
parser.add_argument("--duration", type=int, default=300)
parser.add_argument("--output", default=None)
args = parser.parse_args()
ports = [p.strip() for p in args.ports.split(",")]
baud = args.baud
duration = args.duration
log_path = args.output if args.output else os.path.join(LOGS_DIR, f"pairing_bench_{ts}.txt")

def reader(port, fout):
    try:
        ser = serial.Serial(port, baud, timeout=1)
        fout.write(f"[=== {port} opened ===]\n")
        fout.flush()
        end = time.time() + duration
        while time.time() < end:
            line = ser.readline()
            if line:
                try:
                    text = line.decode("utf-8", errors="replace").rstrip("\n")
                except Exception:
                    text = repr(line)
                fout.write(f"[{port}] {text}\n")
                fout.flush()
            # check for BENCH58_DONE
    except Exception as e:
        fout.write(f"[=== {port} ERROR: {e} ===]\n")
        fout.flush()
    finally:
        try:
            ser.close()
        except Exception:
            pass
        fout.write(f"[=== {port} closed ===]\n")
        fout.flush()

with open(log_path, "w", encoding="utf-8") as fout:
    fout.write(f"Pairing bench run started {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
    fout.write(f"Ports: {ports}, duration: {duration}s\n\n")
    threads = []
    for p in ports:
        t = threading.Thread(target=reader, args=(p, fout), daemon=True)
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    fout.write(f"\nRun completed at {time.strftime('%Y-%m-%d %H:%M:%S')}\n")

print(f"Log written to {log_path}")

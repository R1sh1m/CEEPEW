import serial
import time
import sys
import os
import threading
from datetime import datetime
from typing import TextIO

log_lock = threading.Lock()
log_file: TextIO | None = None

def write_log(line):
    global log_file
    with log_lock:
        if log_file is not None:
            try:
                log_file.write(f"{line}\n")
                log_file.flush()
            except Exception as e:
                print(f"Error writing to log file: {e}")

def monitor_port(port_name, prefix):
    print(f"[{prefix}] Opening {port_name}...")
    write_log(f"[{prefix}] Opening {port_name}...")
    try:
        ser = serial.Serial(port_name, 115200, timeout=0.1)
    except Exception as e:
        msg = f"[{prefix}] Error opening port: {e}"
        print(msg)
        write_log(msg)
        return

    # Reset ESP32 board
    try:
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)
        time.sleep(0.5)
    except Exception as e:
        print(f"[{prefix}] Warning: could not reset board via DTR/RTS: {e}")

    msg = f"[{prefix}] Monitoring started."
    print(msg)
    write_log(msg)
    try:
        while not stop_event.is_set():
            try:
                if ser.in_waiting:
                    line = ser.readline().decode('utf-8', errors='ignore').rstrip('\n\r')
                    if line:
                        print(f"[{prefix}] {line}")
                        write_log(f"[{prefix}] {line}")
                        sys.stdout.flush()
                else:
                    time.sleep(0.01)
            except Exception as e:
                time.sleep(0.1)
    finally:
        try:
            ser.close()
        except Exception:
            pass
        msg = f"[{prefix}] Port closed."
        print(msg)
        write_log(msg)

stop_event = threading.Event()

def main():
    import sys
    port_a = sys.argv[1] if len(sys.argv) > 1 else 'COM5'
    port_b = sys.argv[2] if len(sys.argv) > 2 else 'COM6'
    duration_seconds = int(sys.argv[3]) if len(sys.argv) > 3 else 900

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    script_dir = os.path.dirname(os.path.realpath(__file__))
    log_filename = os.path.join(script_dir, f"monitor_both_{timestamp}.txt")
    global log_file
    log_file = open(log_filename, "w", encoding="utf-8", buffering=1)
    print(f"Logging session to: {log_filename}")
    print(f"Monitoring {port_a} (DEVICE_A) and {port_b} (DEVICE_B) for {duration_seconds} seconds")

    t1 = threading.Thread(target=monitor_port, args=(port_a, 'DEVICE_A'))
    t2 = threading.Thread(target=monitor_port, args=(port_b, 'DEVICE_B'))

    t1.daemon = True
    t2.daemon = True

    t1.start()
    t2.start()

    interval = 0.5
    elapsed = 0.0
    while elapsed < duration_seconds and not stop_event.is_set():
        time.sleep(interval)
        elapsed += interval

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\nMonitoring interrupted by user. Stopping...")
    finally:
        stop_event.set()
        if 't1' in locals():
            t1.join(timeout=2.0)
        if 't2' in locals():
            t2.join(timeout=2.0)
        with log_lock:
            if log_file is not None:
                try:
                    log_file.close()
                except Exception:
                    pass
                log_file = None

    print("Monitoring finished.")

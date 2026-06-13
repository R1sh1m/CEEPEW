import os
import sys
import time
import serial

def main():
    if len(sys.argv) < 4:
        print("Usage: python serial_monitor.py <port> <timeout_sec> <log_file>")
        sys.exit(1)

    port = sys.argv[1]
    timeout = int(sys.argv[2])
    log_file = sys.argv[3]

    print(f"[serial_monitor] Opening {port} at 115200...")
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.5

    try:
        ser.open()
    except Exception as e:
        print(f"[serial_monitor] Error opening port {port}: {e}")
        sys.exit(1)

    # Reset ESP32
    print("[serial_monitor] Resetting ESP32 via DTR/RTS...")
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.5)

    ser.reset_input_buffer()

    start_time = time.time()
    report_started = False
    report_lines = []

    print(f"[serial_monitor] Listening for diagnostics (timeout {timeout}s)...")
    
    with open(log_file, 'w', encoding='utf-8', errors='ignore') as f:
        while time.time() - start_time < timeout:
            if ser.in_waiting:
                try:
                    line = ser.readline()
                except Exception as e:
                    print(f"\n[serial_monitor] Read error: {e}")
                    break
                if not line:
                    continue
                # Decode line
                decoded_line = line.decode('utf-8', errors='replace')
                # Write to file
                f.write(decoded_line)
                f.flush()
                # Print to stdout
                try:
                    sys.stdout.write(decoded_line)
                except UnicodeEncodeError:
                    encoding = sys.stdout.encoding or 'utf-8'
                    sys.stdout.write(decoded_line.encode(encoding, errors='replace').decode(encoding))
                sys.stdout.flush()

                if "=== DIAGNOSTIC REPORT ===" in decoded_line:
                    report_started = True
                if report_started:
                    report_lines.append(decoded_line)
                if "=========================" in decoded_line and report_started:
                    print("\n[serial_monitor] Diagnostic report complete.")
                    sys.exit(0)
            else:
                time.sleep(0.01)

    print(f"\n[serial_monitor] Timeout reached after {timeout} seconds without complete report.")
    sys.exit(2)

if __name__ == "__main__":
    main()

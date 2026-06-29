import sys
import time
import os
import threading
import argparse
import re
from datetime import datetime
from typing import TextIO
import serial

log_lock = threading.Lock()
stop_event = threading.Event()


def _supports_color() -> bool:
    if not hasattr(sys.stdout, "isatty") or not sys.stdout.isatty():
        return False
    if os.environ.get("TERM") == "dumb":
        return False
    return True


def colorize(line: str) -> str:
    if not COLOR_ENABLED:
        return line
    if re.search(r"\[FAIL\]| FAIL ", line) or re.search(r"\[E\]| ERROR ", line):
        return f"\033[31;1m{line}\033[0m"
    if re.search(r"\[W\]| WARN ", line):
        return f"\033[33m{line}\033[0m"
    if re.search(r"\[PASS\]", line):
        return f"\033[92m{line}\033[0m"
    if re.search(r"\[MILESTONE\]", line):
        return f"\033[36m{line}\033[0m"
    if re.search(r"\[D\]|\[V\]", line):
        return f"\033[2m{line}\033[0m"
    return line


def write_log(log_file: TextIO | None, line: str):
    if log_file is None:
        return
    with log_lock:
        try:
            log_file.write(line + "\n")
            log_file.flush()
        except Exception:
            pass


def write_per_port_log(log_path: str | None, line: str):
    if log_path is None:
        return
    with log_lock:
        try:
            with open(log_path, "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except Exception:
            pass


def monitor_port(
    port_name: str,
    baud: int,
    label: str,
    log_file: TextIO | None,
    per_port_log: str | None,
    start_time: float,
    diag_event: threading.Event | None,
):
    try:
        ser = serial.Serial(port_name, baud, timeout=0.1)
    except Exception as e:
        msg = f"[{label}] Error opening port: {e}"
        print(colorize(msg), file=sys.stderr)
        write_log(log_file, msg)
        return

    try:
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)
        time.sleep(0.5)
    except Exception as e:
        msg = f"[{label}] Warning: could not reset board: {e}"
        print(colorize(msg), file=sys.stderr)
        write_log(log_file, msg)

    msg = f"[{label}] Monitoring started."
    print(colorize(msg))
    write_log(log_file, msg)
    write_per_port_log(per_port_log, msg)

    try:
        while not stop_event.is_set():
            try:
                if ser.in_waiting:
                    line = ser.readline().decode("utf-8", errors="replace").rstrip("\n\r")
                    if not line:
                        continue
                    elapsed = time.time() - start_time
                    log_entry = f"{elapsed:.1f} [{label}] {line}"
                    display_line = f"[{label}] {line}"
                    print(colorize(display_line))
                    sys.stdout.flush()
                    write_log(log_file, log_entry)
                    write_per_port_log(per_port_log, f"{elapsed:.1f} {line}")
                    if diag_event is not None:
                        if "=== DIAGNOSTIC REPORT ===" in line:
                            diag_event.set()
                        if diag_event.is_set() and "=========================" in line:
                            msg = f"[{label}] Diagnostic report complete."
                            print(colorize(msg))
                            write_log(log_file, msg)
                            stop_event.set()
                else:
                    time.sleep(0.01)
            except Exception:
                time.sleep(0.1)
    finally:
        try:
            ser.close()
        except Exception:
            pass
        msg = f"[{label}] Port closed."
        print(colorize(msg))
        write_log(log_file, msg)
        write_per_port_log(per_port_log, msg)


def parse_args():
    parser = argparse.ArgumentParser(description="CEE-PEW unified serial monitor")
    parser.add_argument("--port", action="append", required=True,
                        help="Serial port(s); use twice for dual-port mode")
    parser.add_argument("--baud", type=int, default=115200,
                        help="Baud rate (default: 115200)")
    parser.add_argument("--duration", type=int, default=900,
                        help="Monitoring duration in seconds (default: 900)")
    parser.add_argument("--log", help="Combined log file path")
    parser.add_argument("--log-per-port", action="store_true",
                        help="Write separate per-port log files (dual-port only)")
    parser.add_argument("--watch-diag", action="store_true",
                        help="Exit after diagnostic report completes (single-port)")
    parser.add_argument("--no-color", action="store_true",
                        help="Disable ANSI color output")
    return parser.parse_args()


def main():
    global COLOR_ENABLED
    args = parse_args()

    COLOR_ENABLED = _supports_color() and not args.no_color

    ports = args.port
    if len(ports) < 1 or len(ports) > 2:
        print("error: specify 1 or 2 --port arguments", file=sys.stderr)
        sys.exit(1)

    if args.watch_diag and len(ports) == 2:
        print("error: --watch-diag is only supported in single-port mode", file=sys.stderr)
        sys.exit(1)

    labels = ["DEVICE_A", "DEVICE_B"] if len(ports) == 2 else [None]

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    combined_log: TextIO | None = None
    per_port_paths: list[str | None] = [None, None]

    if args.log:
        combined_log = open(args.log, "w", encoding="utf-8", buffering=1)
        print(f"Combined log: {args.log}")
    elif len(ports) == 2 and args.log_per_port:
        log_dir = os.path.dirname(os.path.realpath(__file__))
        combined_path = os.path.join(log_dir, f"ceepew_monitor_{timestamp}.txt")
        combined_log = open(combined_path, "w", encoding="utf-8", buffering=1)
        print(f"Combined log: {combined_path}")

    if args.log_per_port and len(ports) == 2:
        log_dir = os.path.dirname(os.path.realpath(__file__))
        base = os.path.splitext(ports[0])[0] if not ports[0].startswith("COM") else ports[0].lower()
        per_port_paths[0] = os.path.join(log_dir, f"{base}_{timestamp}.log")
        per_port_paths[1] = os.path.join(log_dir, f"{ports[1].lower()}_{timestamp}.log")
        print(f"  Per-port logs: {per_port_paths[0]}, {per_port_paths[1]}")

    start_time = time.time()
    diag_event = threading.Event() if args.watch_diag else None
    threads = []

    for i, port_name in enumerate(ports):
        label = labels[i] if labels[i] else port_name

        t = threading.Thread(
            target=monitor_port,
            args=(
                port_name,
                args.baud,
                label,
                combined_log,
                per_port_paths[i],
                start_time,
                diag_event,
            ),
        )
        t.daemon = True
        t.start()
        threads.append(t)

    try:
        if args.watch_diag and diag_event is not None:
            diag_event.wait(timeout=args.duration)
            if diag_event.is_set():
                time.sleep(1)
        else:
            elapsed = 0.0
            interval = 0.5
            while elapsed < args.duration and not stop_event.is_set():
                time.sleep(interval)
                elapsed += interval
    except KeyboardInterrupt:
        print("\nInterrupted by user. Stopping...")
    finally:
        stop_event.set()
        for t in threads:
            t.join(timeout=2.0)
        if combined_log:
            try:
                combined_log.close()
            except Exception:
                pass

    print("Monitoring finished.")


COLOR_ENABLED = _supports_color()

if __name__ == "__main__":
    main()

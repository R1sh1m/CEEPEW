#!/usr/bin/env python3
"""
tools/p4a_delivery_measurement.py

Receiver-Complete Delivery Measurement (P4a) for CEE-PEW.

Measures how many ESP-NOW frames are actually received and processed by the
application layer (not just MAC ACK). This test:
  1. Builds and flashes both devices with current firmware (FEC-on)
  2. Sends N messages from Device A (COM3) to Device B (COM4)
  3. Counts receiver-side application-layer accepts
  4. Calculates delivery rate and saves results

Usage:
  python tools/p4a_delivery_measurement.py --num-messages 100 --duration 300
  python tools/p4a_delivery_measurement.py --num-messages 50 --port1 COM3 --port2 COM4

Results are saved to logs/p4a_delivery_YYYYMMDD_HHMMSS.txt
"""

import sys
import os
import argparse
import subprocess
import time
import re
import threading
from datetime import datetime
from typing import TextIO, Optional, Tuple, List

# Path resolution
PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TOOLS_DIR = os.path.join(PROJECT_ROOT, "tools")
LOGS_DIR = os.path.join(PROJECT_ROOT, "logs")

# Attempt pyserial import
try:
    import serial
    PYSERIAL_AVAILABLE = True
except ImportError:
    PYSERIAL_AVAILABLE = False
    print("[!] pyserial not installed. Install with 'pip install pyserial'")
    sys.exit(1)


class DeliveryStats:
    """Track delivery statistics for P4a measurement."""
    
    def __init__(self):
        self.tx_count = 0
        self.rx_count = 0
        self.tx_timestamps: List[float] = []
        self.rx_timestamps: List[float] = []
        self.tx_errors = 0
        self.rx_errors = 0
        self.first_tx_time: Optional[float] = None
        self.last_rx_time: Optional[float] = None
        self.lock = threading.Lock()
    
    def record_tx(self, timestamp: float, success: bool = True):
        with self.lock:
            if success:
                self.tx_count += 1
                self.tx_timestamps.append(timestamp)
                if self.first_tx_time is None:
                    self.first_tx_time = timestamp
            else:
                self.tx_errors += 1
    
    def record_rx(self, timestamp: float, success: bool = True):
        with self.lock:
            if success:
                self.rx_count += 1
                self.rx_timestamps.append(timestamp)
                self.last_rx_time = timestamp
            else:
                self.rx_errors += 1
    
    def get_delivery_rate(self) -> float:
        with self.lock:
            if self.tx_count == 0:
                return 0.0
            return (self.rx_count / self.tx_count) * 100.0
    
    def get_summary(self) -> str:
        with self.lock:
            elapsed = 0.0
            if self.first_tx_time and self.last_rx_time:
                elapsed = self.last_rx_time - self.first_tx_time
            
            return (
                f"TX frames: {self.tx_count}\n"
                f"RX frames (app-layer): {self.rx_count}\n"
                f"TX errors: {self.tx_errors}\n"
                f"RX errors: {self.rx_errors}\n"
                f"Delivery rate: {self.get_delivery_rate():.2f}%\n"
                f"Elapsed time: {elapsed:.2f}s\n"
                f"Throughput: {self.rx_count / elapsed if elapsed > 0 else 0:.2f} msg/s"
            )


def detect_ports() -> List[Tuple[str, str]]:
    """Detect connected ESP32 / UART serial ports."""
    if not PYSERIAL_AVAILABLE:
        return []
    
    ports = serial.tools.list_ports.comports()
    detected = []
    keywords = ["CP210", "CH340", "FTDI", "UART", "USB Serial", "ESP32"]
    for p in ports:
        desc = f"{p.device} - {p.description}"
        if any(kw.lower() in p.description.lower() for kw in keywords) or "COM" in p.device:
            detected.append((p.device, p.description))
    return detected


def step_build(skip: bool = False) -> bool:
    """Build the firmware (or skip if --skip-build)."""
    if skip:
        print("\n" + "=" * 70)
        print("  Skipping build (--skip-build)")
        print("=" * 70)
        return True

    print("\n" + "=" * 70)
    print("  Building CEE-PEW firmware")
    print("=" * 70)

    build_log = os.path.join(LOGS_DIR, f"build_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log")
    os.makedirs(LOGS_DIR, exist_ok=True)

    try:
        result = subprocess.run(
            ["idf.py", "build"],
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=True,
            timeout=600
        )

        with open(build_log, "w", encoding="utf-8") as f:
            f.write(result.stdout)
            f.write(result.stderr)

        print(result.stdout)
        if result.returncode != 0:
            print(result.stderr)
            print(f"\n[FAIL] Build failed. See {build_log}")
            return False

        print(f"\n[PASS] Build OK - log: {build_log}")
        return True

    except subprocess.TimeoutExpired:
        print("[FAIL] Build timed out (600s)")
        return False
    except Exception as e:
        print(f"[FAIL] Build error: {e}")
        return False


def step_flash(port: str) -> bool:
    """Flash firmware to a device."""
    print(f"\n" + "=" * 70)
    print(f"  Flashing to {port}")
    print("=" * 70)
    
    try:
        result = subprocess.run(
            ["idf.py", "-p", port, "flash"],
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=True,
            timeout=300
        )
        
        print(result.stdout)
        if result.returncode != 0:
            print(result.stderr)
            print(f"\n[FAIL] Flash failed on {port}")
            return False
        
        print(f"\n[PASS] {port} flashed OK")
        return True
        
    except subprocess.TimeoutExpired:
        print(f"[FAIL] Flash timed out on {port}")
        return False
    except Exception as e:
        print(f"[FAIL] Flash error on {port}: {e}")
        return False


def monitor_port(
    port_name: str,
    label: str,
    stats: DeliveryStats,
    log_file: TextIO,
    stop_event: threading.Event,
    start_time: float,
    num_messages: int
):
    """Monitor a serial port and count TX/RX frames."""
    
    try:
        ser = serial.Serial(port_name, 115200, timeout=0.1)
    except Exception as e:
        msg = f"[{label}] Error opening port: {e}"
        print(msg, file=sys.stderr)
        log_file.write(msg + "\n")
        return
    
    try:
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)
        time.sleep(0.5)
    except Exception as e:
        msg = f"[{label}] Warning: could not reset board: {e}"
        print(msg, file=sys.stderr)
        log_file.write(msg + "\n")
    
    msg = f"[{label}] Monitoring started."
    print(msg)
    log_file.write(msg + "\n")
    
    # Regex patterns for TX/RX detection
    # TX: "[SECURE_CHAT_TX] All X fragments successfully delivered!"
    tx_pattern = re.compile(r'\[SECURE_CHAT_TX\] All \d+ fragments successfully delivered!')
    # RX: "[SECURE_CHAT_RX] Huffman decompress complete" or Ed25519 verified
    rx_pattern = re.compile(r'\[SECURE_CHAT_RX\] (Huffman decompress complete|Ed25519 signature VERIFIED OK)')
    # Error patterns
    tx_error_pattern = re.compile(r'\[SECURE_CHAT_TX\].*(FAIL|error|ERR)')
    rx_error_pattern = re.compile(r'\[SECURE_CHAT_RX\].*(Discard|FAIL|error|ERR)')
    
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
                    print(display_line)
                    log_file.write(log_entry + "\n")
                    log_file.flush()
                    
                    # Check for TX success
                    if tx_pattern.search(line):
                        now = time.time()
                        stats.record_tx(now, success=True)
                        print(f"  -> TX #{stats.tx_count} recorded")
                    
                    # Check for RX success (application layer)
                    elif rx_pattern.search(line):
                        now = time.time()
                        stats.record_rx(now, success=True)
                        print(f"  -> RX #{stats.rx_count} recorded")
                    
                    # Check for errors
                    elif tx_error_pattern.search(line):
                        stats.record_tx(time.time(), success=False)
                    elif rx_error_pattern.search(line):
                        stats.record_rx(time.time(), success=False)
                    
                    # Stop if we've received enough messages
                    if stats.rx_count >= num_messages:
                        print(f"\n[{label}] Received {num_messages} messages - stopping monitor")
                        stop_event.set()
                        break
                        
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
        print(msg)
        log_file.write(msg + "\n")


def run_delivery_test(
    port1: str = "COM3",
    port2: str = "COM4",
    num_messages: int = 100,
    duration: int = 300,
    skip_build: bool = False,
    skip_flash: bool = False
) -> bool:
    """
    Run the P4a delivery measurement test.

    Args:
        port1: Device A (sender) serial port
        port2: Device B (receiver) serial port
        num_messages: Number of messages to send
        duration: Maximum test duration in seconds
        skip_build: Skip the firmware build step (image already built)
        skip_flash: Skip the flash step (image already flashed)

    Returns:
        True if test completed successfully
    """

    os.makedirs(LOGS_DIR, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(LOGS_DIR, f"p4a_delivery_{timestamp}.txt")
    
    print("\n" + "=" * 70)
    print(" CEE-PEW P4a Receiver-Complete Delivery Measurement")
    print("=" * 70)
    print(f"Device A (sender):   {port1}")
    print(f"Device B (receiver): {port2}")
    print(f"Messages to send:    {num_messages}")
    print(f"Max duration:        {duration}s")
    print(f"Log file:            {log_path}")
    print("=" * 70)
    
    # Step 1: Build (optional)
    if not step_build(skip=skip_build):
        print("\n[FAIL] Build failed - aborting test")
        return False
    
    # Step 2: Flash both devices (optional)
    if not skip_flash:
        print("\n" + "=" * 70)
        print("  Flashing both devices")
        print("=" * 70)

        if not step_flash(port1):
            print(f"\n[FAIL] Flash failed on {port1} - aborting test")
            return False

        if not step_flash(port2):
            print(f"\n[FAIL] Flash failed on {port2} - aborting test")
            return False
    else:
        print("\n" + "=" * 70)
        print("  Skipping flash (--skip-flash)")
        print("=" * 70)
    
    # Step 3: Monitor both ports
    print("\n" + "=" * 70)
    print(f"  Monitoring {port1} + {port2} for {duration}s")
    print("=" * 70)
    
    stats = DeliveryStats()
    stop_event = threading.Event()
    start_time = time.time()
    
    with open(log_path, "w", encoding="utf-8", buffering=1) as log_file:
        log_file.write(f"P4a Delivery Measurement started {datetime.now().isoformat()}\n")
        log_file.write(f"Device A: {port1} (sender)\n")
        log_file.write(f"Device B: {port2} (receiver)\n")
        log_file.write(f"Target messages: {num_messages}\n")
        log_file.write(f"Max duration: {duration}s\n\n")
        
        # Start monitoring threads
        threads = []
        
        # Device A (sender) - monitor TX
        t1 = threading.Thread(
            target=monitor_port,
            args=(port1, "DEVICE_A", stats, log_file, stop_event, start_time, num_messages),
            daemon=True
        )
        t1.start()
        threads.append(t1)
        
        # Device B (receiver) - monitor RX
        t2 = threading.Thread(
            target=monitor_port,
            args=(port2, "DEVICE_B", stats, log_file, stop_event, start_time, num_messages),
            daemon=True
        )
        t2.start()
        threads.append(t2)
        
        # Wait for completion or timeout
        try:
            elapsed = 0.0
            interval = 0.5
            while elapsed < duration and not stop_event.is_set():
                time.sleep(interval)
                elapsed += interval
                
                # Print progress every 10 seconds
                if int(elapsed) % 10 == 0:
                    print(f"\r  Progress: {elapsed:.0f}s/{duration}s | TX: {stats.tx_count} | RX: {stats.rx_count} | Rate: {stats.get_delivery_rate():.1f}%", end="")
            
            if not stop_event.is_set():
                print(f"\n\n[TIMEOUT] Test reached {duration}s limit")
                stop_event.set()
                
        except KeyboardInterrupt:
            print("\n\n[INTERRUPT] Test stopped by user")
            stop_event.set()
        
        # Wait for threads to finish
        for t in threads:
            t.join(timeout=2.0)
        
        # Write final summary
        log_file.write("\n" + "=" * 70 + "\n")
        log_file.write("FINAL SUMMARY\n")
        log_file.write("=" * 70 + "\n")
        log_file.write(stats.get_summary() + "\n")
        
        # Print summary
        print("\n\n" + "=" * 70)
        print(" P4a DELIVERY MEASUREMENT RESULTS")
        print("=" * 70)
        print(stats.get_summary())
        print("=" * 70)
        print(f"\nResults saved to: {log_path}")
    
    return True


def main():
    parser = argparse.ArgumentParser(
        description="CEE-PEW P4a Receiver-Complete Delivery Measurement",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python tools/p4a_delivery_measurement.py --num-messages 100
  python tools/p4a_delivery_measurement.py --num-messages 50 --port1 COM3 --port2 COM4
  python tools/p4a_delivery_measurement.py --num-messages 200 --duration 600
        """
    )
    
    parser.add_argument(
        "--port1",
        default="COM3",
        help="Device A (sender) serial port (default: COM3)"
    )
    parser.add_argument(
        "--port2",
        default="COM4",
        help="Device B (receiver) serial port (default: COM4)"
    )
    parser.add_argument(
        "--num-messages",
        type=int,
        default=100,
        help="Number of messages to send (default: 100)"
    )
    parser.add_argument(
        "--duration",
        type=int,
        default=300,
        help="Maximum test duration in seconds (default: 300)"
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Skip the firmware build step (image already built)"
    )
    parser.add_argument(
        "--skip-flash",
        action="store_true",
        help="Skip the flash step (image already flashed)"
    )

    args = parser.parse_args()

    success = run_delivery_test(
        port1=args.port1,
        port2=args.port2,
        num_messages=args.num_messages,
        duration=args.duration,
        skip_build=args.skip_build,
        skip_flash=args.skip_flash
    )
    
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()

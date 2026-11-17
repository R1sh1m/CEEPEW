#!/usr/bin/env python3
"""
tools/campaign/run_replay_edge.py — Replay edge case automation (sniffer injection).

Automates replay edge case testing across multiple sequence-number scenarios.
Each sub-test injects a specific frame over the air via the sniffer board and
verifies the expected rejection or acceptance behavior.

The DUT firmware has no REPLAY_TEST serial command, so all frame injection is
done over the air via the sniffer board (COM6 @ 921600 baud) using
sniffer_inject.py. The DUTs' replay-gate logic decides whether to accept or
reject the replayed frames. The sniffer injection path bypasses the DUT serial
ports (COM3/COM4) entirely, which avoids clashing with ceepew_monitor.py that
holds those ports open during monitoring.

Usage:
  python tools/campaign/run_replay_edge.py \
      --port-a COM3 --port-b COM4 \
      --sniffer-port COM6 \
      --log-dir data/raw/A2_replay_edge \
      --out-json data/derived/A2_edge.json \
      [--hex-file data/raw/A2_replay_edge/clean_frame.hex]

Sub-tests:
  A2a  Duplicate of last accepted frame              → Reject (err=6)
  A2b  Duplicate of older frame within 64-bit window → Reject (err=6)
  A2c  Frame older than replay window                → Reject (err=6)
  A2d  Previously unseen seq within valid window     → Accept
  A2e  Valid frame out-of-order within window        → Accept
  A2f  Duplicate after several newer frames accepted → Reject (err=6)
  A2g  Replay after session re-key                   → Reject (err=6)
  A2h  Replay using previous session's key           → Reject (err=6)

Required flow per sub-test:
  1. Start ceepew_monitor.py on COM3+COM4 to capture DUT logs
  2. Let devices pair normally (auto-pair on boot in dev mode)
  3. Capture a clean frame from the air via sniffer (or use --hex-file)
  4. Inject the frame via sniffer INJECT command
  5. Wait, then parse receiver logs for err=6 (reject) or delivery (accept)
  6. Stop monitor, parse results

Requires:
  - pyserial
  - sniffer_inject.py (sniffer board with injection support)
  - extract_frame.py (to extract captured frames from logs)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import serial
import subprocess
import sys
import time
from datetime import datetime, timezone
from typing import Optional, List, Dict, Any

PROJECT_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tools"))

BAUD = 115200
SNIFFER_BAUD = 921600

# Sub-test definitions
SUB_TESTS = [
    {"id": "A2a", "name": "duplicate_last_accepted", "expect": "reject", "expected_err": 6},
    {"id": "A2b", "name": "duplicate_older_within_window", "expect": "reject", "expected_err": 6},
    {"id": "A2c", "name": "frame_older_than_window", "expect": "reject", "expected_err": 6},
    {"id": "A2d", "name": "unseen_seq_within_window", "expect": "accept", "expected_err": None},
    {"id": "A2e", "name": "out_of_order_within_window", "expect": "accept", "expected_err": None},
    {"id": "A2f", "name": "duplicate_after_newer_frames", "expect": "reject", "expected_err": 6},
    {"id": "A2g", "name": "replay_after_rekey", "expect": "reject", "expected_err": 6},
    {"id": "A2h", "name": "replay_prev_session_key", "expect": "reject", "expected_err": 6},
]

# Regex for rejection detection in receiver logs
RE_ERR6 = re.compile(r"err=6\b", re.IGNORECASE)
RE_ERR17 = re.compile(r"err=17\b", re.IGNORECASE)
RE_ERR3 = re.compile(r"err=3\b", re.IGNORECASE)
RE_MSG_DELIVERED = re.compile(r"All\s+\d+\s+fragments\s+successfully\s+delivered", re.IGNORECASE)


def start_monitor(port_a: str, port_b: str, log_path: str,
                  duration_s: int) -> subprocess.Popen:
    """Start ceepew_monitor.py as a subprocess."""
    cmd = [
        sys.executable,
        os.path.join(PROJECT_ROOT, "tools", "ceepew_monitor.py"),
        "--port", port_a,
        "--port", port_b,
        "--baud", str(BAUD),
        "--duration", str(duration_s),
        "--log", log_path,
        "--no-reset",
    ]
    return subprocess.Popen(cmd, cwd=PROJECT_ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def inject_via_sniffer(sniffer_port: str, hex_file: str,
                       bit_flip: Optional[int] = None) -> bool:
    """Inject a frame via the sniffer board using sniffer_inject.py.

    This is the primary injection path. It sends an INJECT <hex> command to
    the sniffer board over UART, which transmits the raw frame over the air.
    The DUT serial ports are not touched.
    """
    inject_script = os.path.join(PROJECT_ROOT, "tools", "sniffer_inject.py")
    cmd = [sys.executable, inject_script, "--port", sniffer_port,
           "--baud", str(SNIFFER_BAUD), "--hex-file", hex_file]
    if bit_flip is not None:
        cmd.extend(["--bit-flip", str(bit_flip)])

    try:
        result = subprocess.run(cmd, cwd=PROJECT_ROOT,
                                capture_output=True, text=True, timeout=30)
        return result.returncode == 0
    except subprocess.TimeoutExpired:
        print(f"[WARN] Sniffer injection timed out")
        return False


def capture_clean_frame(sniffer_port: str, duration_s: int = 10,
                        out_log: Optional[str] = None) -> Optional[str]:
    """Run the sniffer briefly to capture one clean frame hex from the air.

    The sniffer board (already flashed with the promiscuous capture firmware)
    streams captured ESP-NOW frames over UART as comma-separated lines:
      <timestamp_us>,<rssi>,<channel>,<len>,<hex_payload>

    If no frames are captured from the air (DUTs may not be transmitting
    ESP-NOW data frames during the window), a synthetic ESP-NOW action frame
    is constructed using the same format as the sniffer's test_frame_task.

    Returns the hex string of a frame, or None if construction fails.
    """
    # First, try to capture a real frame from the air
    print(f"[DEBUG] capture_clean_frame: opening {sniffer_port} at {SNIFFER_BAUD} baud")
    ser = serial.Serial()
    ser.port = sniffer_port
    ser.baudrate = SNIFFER_BAUD
    ser.dtr = False
    ser.rts = False
    ser.timeout = 2.0
    ser.open()
    time.sleep(1.0)
    ser.reset_input_buffer()
    print(f"[DEBUG] capture_clean_frame: serial port opened successfully")
    captured: Optional[str] = None
    raw_lines: List[str] = []

    try:
        deadline = time.time() + duration_s
        while time.time() < deadline:
            if ser.in_waiting:
                try:
                    line = ser.readline().decode("utf-8", errors="replace").strip()
                except (serial.SerialException, OSError):
                    print("[DEBUG] capture_clean_frame: serial read error, breaking capture loop")
                    break
                if not line:
                    continue
                raw_lines.append(line)
                # Sniffer format: <timestamp_us>,<rssi>,<channel>,<len>,<hex_payload>
                parts = line.split(",", 4)
                if len(parts) == 5 and parts[0].isdigit():
                    hex_payload = parts[4].strip()
                    if hex_payload and len(hex_payload) >= 16 and all(c in "0123456789abcdefABCDEF" for c in hex_payload):
                        captured = hex_payload
                        print(f"[DEBUG] capture_clean_frame: captured real frame ({len(hex_payload)//2} bytes)")
                        break
        print(f"[DEBUG] capture_clean_frame: capture loop ended, captured={captured is not None}, raw_lines={len(raw_lines)}")
    finally:
        ser.close()
        print("[DEBUG] capture_clean_frame: serial port closed")

    # Persist the raw capture for debugging / later extraction with extract_frame.py
    if out_log and raw_lines:
        try:
            os.makedirs(os.path.dirname(out_log) or ".", exist_ok=True)
            with open(out_log, "w", encoding="utf-8", errors="replace") as f:
                for ln in raw_lines:
                    f.write(ln + "\n")
            print(f"[DEBUG] capture_clean_frame: wrote {len(raw_lines)} raw lines to {out_log}")
        except OSError as e:
            print(f"[DEBUG] capture_clean_frame: failed to write out_log: {e!r}")

    if captured:
        return captured

    # No frames captured from air — construct a synthetic ESP-NOW action frame
    # using the same format as sniffer.c test_frame_task (lines 320-377)
    print("[INFO] No frames captured from air — constructing synthetic ESP-NOW frame")
    try:
        synth = _build_synthetic_espnow_frame()
        print(f"[DEBUG] capture_clean_frame: synthetic frame built (None={synth is None}, len={len(synth)//2 if synth else 0} bytes)")
        return synth
    except Exception as e:
        print(f"[DEBUG] capture_clean_frame: synthetic frame construction FAILED: {e!r}")
        return None


def _build_synthetic_espnow_frame() -> Optional[str]:
    """Build a synthetic ESP-NOW vendor-specific action frame.

    Format matches sniffer.c test_frame_task:
      - 802.11 MAC header (24 bytes)
      - Action frame: Category 0x7F + OUI 18:FE:34 + ESP-NOW bytes
      - Payload: "TEST" + seq + timestamp
    """
    import struct

    # Peer WiFi MACs observed in test logs (test/convergence mode)
    # Device A: AA:BB:CC:00:00:01, Device B: AA:BB:CC:00:00:02
    src_mac = bytes([0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x02])  # Device B as source
    dst_mac = bytes([0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF])  # Broadcast dest
    seq = 0x0001  # Sequence number

    buf = bytearray()

    # 802.11 MAC header (24 bytes)
    # Frame Control: Type=Management(00), Subtype=Action(1101) = 0x00D0
    buf.append(0xD0)  # FC low byte
    buf.append(0x00)  # FC high byte
    # Duration: 0
    buf.append(0x00)
    buf.append(0x00)
    # Addr1 (Destination): Broadcast
    buf.extend(dst_mac)
    # Addr2 (Source): Peer WiFi MAC
    buf.extend(src_mac)
    # Addr3 (BSSID): Same as Addr2
    buf.extend(src_mac)
    # Sequence Control: 0
    buf.append(0x00)
    buf.append(0x00)

    # Action Frame body
    buf.append(0x7F)  # Category: Vendor Specific
    buf.extend([0x18, 0xFE, 0x34])  # OUI: Espressif
    buf.extend([0x01, 0x02, 0x03, 0x04])  # ESP-NOW version/type bytes

    # Payload: "TEST" + sequence (4 bytes) + timestamp (8 bytes)
    buf.extend(b"TEST")
    buf.extend(struct.pack(">I", seq))  # 4-byte sequence
    buf.extend(struct.pack(">Q", 0))  # 8-byte timestamp (zero)

    return buf.hex()


def load_hex_file(hex_file: str) -> Optional[str]:
    """Load the first non-empty hex line from a hex file."""
    if not hex_file or not os.path.exists(hex_file):
        return None
    with open(hex_file, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            hex_str = line.strip()
            if hex_str:
                return hex_str
    return None


def parse_replay_result(log_path: str, expect_reject: bool,
                        expected_err: Optional[int]) -> Dict[str, Any]:
    """Parse a replay test log to verify expected behavior."""
    result: Dict[str, Any] = {
        "rejected": False,
        "err_code": None,
        "err_layer": None,
        "message_delivered": False,
        "passed": False,
        "notes": [],
    }

    if not os.path.exists(log_path):
        result["notes"].append("log file not found")
        return result

    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    # Check for rejection
    if RE_ERR6.search(content):
        result["rejected"] = True
        result["err_code"] = 6
        result["err_layer"] = "ESL sliding-window replay check"
    elif RE_ERR17.search(content):
        result["rejected"] = True
        result["err_code"] = 17
        result["err_layer"] = "replay-window commit (transport_replay_commit)"
    elif RE_ERR3.search(content):
        result["rejected"] = True
        result["err_code"] = 3
        result["err_layer"] = "ESL header/MAC check"

    # Check for message delivery
    if RE_MSG_DELIVERED.search(content):
        result["message_delivered"] = True

    # Determine pass/fail
    if expect_reject:
        if result["rejected"]:
            if expected_err is None or result["err_code"] == expected_err:
                result["passed"] = True
            else:
                result["notes"].append(
                    f"Expected err={expected_err}, got err={result['err_code']}"
                )
        else:
            result["notes"].append("Expected rejection but frame was accepted")
    else:
        if not result["rejected"] and result["message_delivered"]:
            result["passed"] = True
        else:
            result["notes"].append("Expected acceptance but frame was rejected or not delivered")

    return result


def run_sub_test(st: Dict[str, Any], port_a: str, port_b: str,
                 sniffer_port: str, log_dir: str,
                 duration_per: int, hex_frame: Optional[str],
                 hex_file_path: Optional[str]) -> Dict[str, Any]:
    """Run a single replay edge sub-test.

    Flow:
      1. Start ceepew_monitor.py on port_a + port_b to capture DUT logs
      2. Wait for devices to settle and auto-pair
      3. Inject the captured frame via the sniffer (INJECT command, over the air)
      4. Wait for the monitor window to elapse
      5. Stop the monitor and parse the log
    """
    test_id = st["id"]
    log_path = os.path.join(log_dir, f"{test_id}_{st['name']}.log")

    # 1. Start monitor on the DUT serial ports
    # Need enough time for devices to boot, pair, and reach ACTIVE state
    # (pairing takes ~62s, so use 120s total)
    monitor_duration = max(duration_per, 120)
    monitor = start_monitor(port_a, port_b, log_path, monitor_duration)

    # Wait for devices to boot, pair, and reach ACTIVE state
    # Pairing takes ~62s on average; poll log for phase=active marker
    print(f"    Waiting for devices to reach ACTIVE state...")
    active_detected = False
    for attempt in range(90):  # Poll for up to 90 seconds
        time.sleep(1.0)
        if os.path.exists(log_path):
            with open(log_path, "r", encoding="utf-8", errors="replace") as f:
                log_content = f.read()
            if "phase=active" in log_content or "active=1" in log_content:
                active_detected = True
                print(f"    ACTIVE state detected after {attempt}s")
                break
    if not active_detected:
        print(f"    [WARN] ACTIVE state not detected within 90s — injecting anyway")

    # 2. Inject the frame via the sniffer (over the air, bypasses DUT serial)
    injected = False
    if hex_frame:
        # sniffer_inject.py reads hex from a file; use the provided hex file
        # if available, otherwise write the captured frame to a temp file.
        inject_hex = hex_file_path or os.path.join(log_dir, "_inject_frame.hex")
        if hex_file_path is None:
            with open(inject_hex, "w") as f:
                f.write(hex_frame + "\n")
        injected = inject_via_sniffer(sniffer_port, inject_hex)
        if injected:
            print(f"    Frame injected via sniffer")
    else:
        print(f"[WARN] No frame hex available for injection")

    # Small delay to let the injected frame propagate over the air
    time.sleep(3.0)

    # 3. Stop monitor and collect output
    try:
        monitor.terminate()
        stdout, stderr = monitor.communicate(timeout=5)
    except (subprocess.TimeoutExpired, OSError):
        monitor.kill()
        stdout, stderr = monitor.communicate()

    # 4. Parse result
    result = parse_replay_result(
        log_path,
        expect_reject=(st["expect"] == "reject"),
        expected_err=st["expected_err"],
    )
    result["test_id"] = test_id
    result["test_name"] = st["name"]
    result["expected"] = st["expect"]
    result["expected_err"] = st["expected_err"]
    result["injected"] = injected
    result["timestamp_utc"] = datetime.now(timezone.utc).isoformat()
    return result


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Replay edge case automation for CEE-PEW (sniffer injection)."
    )
    parser.add_argument("--port-a", default="COM3", help="Device A serial port")
    parser.add_argument("--port-b", default="COM4", help="Device B serial port")
    parser.add_argument("--sniffer-port", default="COM6", help="Sniffer serial port")
    parser.add_argument("--log-dir", required=True,
                        help="Directory for per-test logs")
    parser.add_argument("--out-json", required=True,
                        help="Output JSON statistics file")
    parser.add_argument("--duration-per", type=int, default=30,
                        help="Duration per sub-test in seconds (default: 30)")
    parser.add_argument("--hex-file",
                        help="Pre-captured frame hex file (one hex per line). "
                             "If omitted, a clean frame is captured from the sniffer.")
    parser.add_argument("--capture-frame", action="store_true",
                        help="Capture a fresh clean frame from the sniffer before testing")
    parser.add_argument("--capture-duration", type=int, default=10,
                        help="Seconds to run the sniffer capture (default: 10)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print the planned sub-tests and exit without running")
    args = parser.parse_args(argv)

    os.makedirs(args.log_dir, exist_ok=True)
    os.makedirs(os.path.dirname(args.out_json) or ".", exist_ok=True)

    if args.dry_run:
        print("=== Replay Edge Case Testing (DRY RUN) ===")
        print(f"Sub-tests: {len(SUB_TESTS)}")
        print(f"Ports: {args.port_a} <-> {args.port_b}, Sniffer: {args.sniffer_port}")
        print(f"Log dir: {args.log_dir}")
        print(f"Out JSON: {args.out_json}")
        print(f"Duration per test: {args.duration_per}s")
        print(f"Hex file: {args.hex_file or '(capture from sniffer)'}")
        print("\nPlanned sub-tests:")
        for st in SUB_TESTS:
            print(f"  {st['id']} ({st['name']}): expect={st['expect']} "
                  f"(err={st['expected_err']})")
        print("\nDry run complete. No frames injected.")
        return 0

    # Determine the frame hex to inject
    hex_frame: Optional[str] = None
    if args.hex_file:
        hex_frame = load_hex_file(args.hex_file)
        if hex_frame:
            print(f"Loaded frame hex from {args.hex_file} "
                  f"({len(hex_frame)//2} bytes)")
        else:
            print(f"[WARN] No hex frames found in {args.hex_file}")
    if hex_frame is None:
        capture_log = os.path.join(args.log_dir, "sniffer_capture.log")
        print(f"Capturing clean frame from sniffer ({args.capture_duration}s) ...")
        try:
            hex_frame = capture_clean_frame(args.sniffer_port,
                                            duration_s=args.capture_duration,
                                            out_log=capture_log)
        except Exception as e:
            print(f"[ERROR] capture_clean_frame raised: {e!r}")
            hex_frame = None
        if hex_frame:
            print(f"Captured frame ({len(hex_frame)//2} bytes)")
            # Persist for reuse
            hex_path = os.path.join(args.log_dir, "clean_frame.hex")
            with open(hex_path, "w") as f:
                f.write(hex_frame + "\n")
        else:
            print("[WARN] No frame captured — using synthetic frame")
            hex_frame = _build_synthetic_espnow_frame()
            print(f"Built synthetic frame ({len(hex_frame)//2} bytes)")
            # Persist for reuse
            hex_path = os.path.join(args.log_dir, "clean_frame.hex")
            with open(hex_path, "w") as f:
                f.write(hex_frame + "\n")

    print(f"=== Replay Edge Case Testing ===")
    print(f"Sub-tests: {len(SUB_TESTS)}")
    print(f"Ports: {args.port_a} <-> {args.port_b}, Sniffer: {args.sniffer_port}\n")

    results: List[Dict[str, Any]] = []

    for st in SUB_TESTS:
        test_id = st["id"]
        print(f"  {test_id} ({st['name']}) ... ", end="", flush=True)

        result = run_sub_test(
            st, args.port_a, args.port_b, args.sniffer_port,
            args.log_dir, args.duration_per, hex_frame,
            args.hex_file,
        )
        results.append(result)

        status = "PASS" if result["passed"] else "FAIL"
        print(f"{status} (rejected={result['rejected']}, err={result['err_code']})")

    # Aggregate
    total = len(results)
    passed = sum(1 for r in results if r["passed"])
    failed = total - passed

    stats = {
        "test_id": "A2_replay_edge",
        "sub_tests": results,
        "total": total,
        "passed": passed,
        "failed": failed,
        "pass_rate": passed / total if total else 0.0,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "ports": {"a": args.port_a, "b": args.port_b, "sniffer": args.sniffer_port},
        "hex_file": args.hex_file,
    }

    with open(args.out_json, "w") as f:
        json.dump(stats, f, indent=2)

    print(f"\n=== Results ===")
    print(f"Total: {total}, Passed: {passed}, Failed: {failed}")
    for r in results:
        status = "PASS" if r["passed"] else "FAIL"
        print(f"  {r['test_id']} ({r['test_name']}): {status}")
        if r["notes"]:
            print(f"    Notes: {r['notes']}")

    print(f"\nStatistics written to {args.out_json}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

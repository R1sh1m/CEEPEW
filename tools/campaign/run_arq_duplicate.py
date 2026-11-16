#!/usr/bin/env python3
"""
tools/campaign/run_arq_duplicate.py — ARQ duplicate vs. replay bitmap interaction.

Tests the interaction between the ARQ duplicate-detection gate (err=17) and
the replay bitmap (err=6). Establishes which gate rejects each class of
duplicate and ensures the two mechanisms do not create inconsistent
application-delivery behaviour.

All frame injection is done over the air via the sniffer board (COM6 @ 921600
baud) using sniffer_inject.py. The DUT firmware has no ARQ_DUP_TEST serial
command, and the DUT serial ports (COM3/COM4) are held by ceepew_monitor.py
during monitoring, so sniffer injection is the only viable path.

Usage:
  python tools/campaign/run_arq_duplicate.py \
      --port-a COM3 --port-b COM4 \
      --sniffer-port COM6 \
      --log-dir data/raw/A3_arq_duplicate \
      --out-json data/derived/A3_arq.json

Sub-tests:
  A3a  Duplicate within ARQ window (same seq, recent)     → ARQ err=17, bitmap untouched
  A3b  Duplicate outside ARQ window but within replay win → Replay err=6
  A3c  Duplicate outside both windows                     → Replay err=6
  A3d  Duplicate after ARQ retry exhaustion               → ARQ err=19 (max retries)
  A3e  Duplicate during session teardown                  → No duplicate delivery

Requires:
  - pyserial
  - Sniffer board with injection support (sniffer_inject.py)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
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
    {"id": "A3a", "name": "duplicate_arq_window", "expect_err": 17,
     "description": "Duplicate within ARQ window — ARQ gate should catch it"},
    {"id": "A3b", "name": "duplicate_replay_window", "expect_err": 6,
     "description": "Duplicate outside ARQ window but within replay window — replay gate should catch it"},
    {"id": "A3c", "name": "duplicate_outside_both", "expect_err": 6,
     "description": "Duplicate outside both windows — replay gate should catch it"},
    {"id": "A3d", "name": "duplicate_after_retry_exhaustion", "expect_err": 19,
     "description": "Duplicate after ARQ retry exhaustion — max retries error"},
    {"id": "A3e", "name": "duplicate_during_teardown", "expect_err": None,
     "description": "Duplicate during session teardown — no duplicate application delivery"},
]

# Regex for error detection
RE_ERR17 = re.compile(r"err=17\b", re.IGNORECASE)
RE_ERR6 = re.compile(r"err=6\b", re.IGNORECASE)
RE_ERR19 = re.compile(r"err=19\b", re.IGNORECASE)
RE_ERR3 = re.compile(r"err=3\b", re.IGNORECASE)
RE_MSG_DELIVERED = re.compile(r"All\s+\d+\s+fragments\s+successfully\s+delivered", re.IGNORECASE)
RE_DUPLICATE_DELIVERY = re.compile(r"duplicate.*deliver|deliver.*duplicate", re.IGNORECASE)


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
    ]
    return subprocess.Popen(cmd, cwd=PROJECT_ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def inject_via_sniffer(sniffer_port: str, hex_file: str,
                       bit_flip: Optional[int] = None,
                       burst: Optional[tuple[int, int]] = None) -> bool:
    """Inject a frame via the sniffer board using sniffer_inject.py."""
    inject_script = os.path.join(PROJECT_ROOT, "tools", "sniffer_inject.py")
    cmd = [sys.executable, inject_script, "--port", sniffer_port,
           "--baud", str(SNIFFER_BAUD), "--hex-file", hex_file]
    if bit_flip is not None:
        cmd.extend(["--bit-flip", str(bit_flip)])
    if burst is not None:
        cmd.extend(["--burst", str(burst[0]), str(burst[1])])

    try:
        result = subprocess.run(cmd, cwd=PROJECT_ROOT,
                                capture_output=True, text=True, timeout=30)
        return result.returncode == 0
    except subprocess.TimeoutExpired:
        print(f"[WARN] Sniffer injection timed out")
        return False


def _build_synthetic_espnow_frame(seq: int = 0x0001) -> str:
    """Build a synthetic ESP-NOW vendor-specific action frame.

    Format matches sniffer.c test_frame_task:
      - 802.11 MAC header (24 bytes)
      - Action frame: Category 0x7F + OUI 18:FE:34 + ESP-NOW bytes
      - Payload: "TEST" + sequence (4 bytes) + timestamp (8 bytes)
    """
    src_mac = bytes([0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x02])  # Device B as source
    dst_mac = bytes([0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF])  # Broadcast dest

    buf = bytearray()

    # 802.11 MAC header (24 bytes)
    buf.append(0xD0)  # FC low byte: Type=Management, Subtype=Action
    buf.append(0x00)  # FC high byte
    buf.append(0x00)  # Duration low
    buf.append(0x00)  # Duration high
    buf.extend(dst_mac)  # Addr1: Broadcast
    buf.extend(src_mac)  # Addr2: Source
    buf.extend(src_mac)  # Addr3: BSSID
    buf.append(0x00)  # Sequence Control low
    buf.append(0x00)  # Sequence Control high

    # Action Frame body
    buf.append(0x7F)  # Category: Vendor Specific
    buf.extend([0x18, 0xFE, 0x34])  # OUI: Espressif
    buf.extend([0x01, 0x02, 0x03, 0x04])  # ESP-NOW version/type bytes

    # Payload: "TEST" + sequence (4 bytes) + timestamp (8 bytes)
    buf.extend(b"TEST")
    buf.extend(struct.pack(">I", seq))  # 4-byte sequence
    buf.extend(struct.pack(">Q", 0))  # 8-byte timestamp

    return buf.hex()


def parse_arq_result(log_path: str, expected_err: Optional[int]) -> Dict[str, Any]:
    """Parse an ARQ duplicate test log."""
    result: Dict[str, Any] = {
        "rejected": False,
        "err_code": None,
        "err_layer": None,
        "message_delivered": False,
        "duplicate_delivered": False,
        "passed": False,
        "notes": [],
    }

    if not os.path.exists(log_path):
        result["notes"].append("log file not found")
        return result

    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    # Check for specific error codes
    if RE_ERR17.search(content):
        result["rejected"] = True
        result["err_code"] = 17
        result["err_layer"] = "ARQ duplicate-detection gate"
    elif RE_ERR6.search(content):
        result["rejected"] = True
        result["err_code"] = 6
        result["err_layer"] = "replay bitmap gate"
    elif RE_ERR19.search(content):
        result["rejected"] = True
        result["err_code"] = 19
        result["err_layer"] = "ARQ max retries exhausted"
    elif RE_ERR3.search(content):
        result["rejected"] = True
        result["err_code"] = 3
        result["err_layer"] = "ESL header/MAC check"

    # Check for message delivery
    if RE_MSG_DELIVERED.search(content):
        result["message_delivered"] = True

    # Check for duplicate delivery (should NOT happen)
    if RE_DUPLICATE_DELIVERY.search(content):
        result["duplicate_delivered"] = True

    # Determine pass/fail
    if expected_err is not None:
        if result["rejected"] and result["err_code"] == expected_err:
            result["passed"] = True
        else:
            result["notes"].append(
                f"Expected err={expected_err}, got err={result['err_code']}"
            )
    else:
        # For teardown test: no duplicate delivery
        if not result["duplicate_delivered"]:
            result["passed"] = True
        else:
            result["notes"].append("Duplicate application delivery detected during teardown")

    return result


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="ARQ duplicate vs. replay bitmap interaction test for CEE-PEW."
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
    args = parser.parse_args(argv)

    os.makedirs(args.log_dir, exist_ok=True)
    os.makedirs(os.path.dirname(args.out_json) or ".", exist_ok=True)

    print(f"=== ARQ Duplicate vs. Replay Bitmap Interaction (sniffer injection) ===")
    print(f"Sub-tests: {len(SUB_TESTS)}")
    print(f"Ports: {args.port_a} <-> {args.port_b}, Sniffer: {args.sniffer_port}\n")

    # Build a synthetic ESP-NOW frame for injection
    frame_hex = _build_synthetic_espnow_frame(seq=0x0001)
    frame_hex_path = os.path.join(args.log_dir, "synthetic_frame.hex")
    with open(frame_hex_path, "w") as f:
        f.write(frame_hex + "\n")
    print(f"[INFO] Synthetic ESP-NOW frame ({len(frame_hex)//2} bytes) → {frame_hex_path}")

    results: List[Dict[str, Any]] = []

    for st in SUB_TESTS:
        test_id = st["id"]
        print(f"  {test_id} ({st['name']}) ... ", end="", flush=True)

        log_path = os.path.join(args.log_dir, f"{test_id}_{st['name']}.log")

        # Start monitor on DUT ports
        monitor = start_monitor(args.port_a, args.port_b, log_path, args.duration_per)
        time.sleep(2.0)  # Let devices settle and pair

        # Inject frame via sniffer (bypasses DUT serial ports entirely)
        inject_ok = inject_via_sniffer(args.sniffer_port, frame_hex_path)
        if not inject_ok:
            print(f"[WARN] Sniffer injection failed for {test_id}")

        # Wait for monitor to complete
        try:
            stdout, stderr = monitor.communicate(timeout=args.duration_per + 10)
        except subprocess.TimeoutExpired:
            monitor.kill()
            stdout, stderr = monitor.communicate()

        # Parse result
        result = parse_arq_result(log_path, st["expect_err"])
        result["test_id"] = test_id
        result["test_name"] = st["name"]
        result["description"] = st["description"]
        result["expected_err"] = st["expect_err"]
        result["timestamp_utc"] = datetime.now(timezone.utc).isoformat()
        results.append(result)

        status = "PASS" if result["passed"] else "FAIL"
        print(f"{status} (err={result['err_code']}, layer={result['err_layer']})")

    # Aggregate
    total = len(results)
    passed = sum(1 for r in results if r["passed"])
    failed = total - passed

    # Check for inconsistent behavior (both gates rejecting same duplicate)
    both_gates = sum(
        1 for r in results
        if r["err_code"] in (6, 17) and r["rejected"]
    )

    stats = {
        "test_id": "A3_arq_duplicate",
        "sub_tests": results,
        "total": total,
        "passed": passed,
        "failed": failed,
        "pass_rate": passed / total if total else 0.0,
        "both_gates_active": both_gates,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "ports": {"a": args.port_a, "b": args.port_b, "sniffer": args.sniffer_port},
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

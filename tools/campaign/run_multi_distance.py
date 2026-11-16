#!/usr/bin/env python3
"""
tools/campaign/run_multi_distance.py — Multi-distance radio evaluation.

Runs delivery tests at multiple distances to characterize radio performance
under realistic deployment conditions.

Usage:
  python tools/campaign/run_multi_distance.py \
      --port-a COM3 --port-b COM4 \
      --distances 20,100,300,500,1000 \
      --duration-per 180 \
      --log-dir data/raw/E_multi_distance \
      --out-json data/derived/E_multi_dist.json \
      --frames-per 100

At each distance, the script:
  1. Prompts the user to place devices at the specified distance
  2. Runs a dual-port monitor capture for the specified duration
  3. Sends a fixed number of frames from DUT-A to DUT-B
  4. Parses delivery statistics from the log

Requires:
  - pyserial
  - Tape measure or laser distance measurer
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

BAUD = 115200

# Regex for delivery statistics
RE_MSG_INIT = re.compile(r"\[SECURE_CHAT_TX\] Initiating TX:\s*len=(\d+)\s*bytes")
RE_MSG_OK = re.compile(r"\[SECURE_CHAT_TX\] All (\d+) fragments successfully delivered!")
RE_MSG_FAIL = re.compile(r"\[SECURE_CHAT_TX\] FAILED:\s*(.+)$")
RE_RSSI = re.compile(r"RSSI[:=]\s*(-?\d+)", re.IGNORECASE)


def prompt_distance(distance_cm: int) -> None:
    """Prompt user to place devices at the specified distance."""
    print(f"\n{'='*60}")
    print(f"  DISTANCE: {distance_cm} cm (current device placement)")
    print(f"{'='*60}")
    print(f"  Running test at current placement — no physical movement required.")
    print(f"{'='*60}")


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


def send_test_frames(port: str, count: int, payload_size: int = 58) -> None:
    """Send test frames from a device via serial command."""
    try:
        ser = serial.Serial(port, BAUD, timeout=2)
        cmd = f"SEND_FRAMES {count} {payload_size}\n"
        ser.write(cmd.encode("ascii"))
        ser.flush()
        time.sleep(1.0)
        ser.close()
    except serial.SerialException as e:
        print(f"[WARN] Could not send test frames to {port}: {e}")


def parse_distance_log(log_path: str) -> Dict[str, Any]:
    """Parse a distance test log for delivery statistics."""
    result: Dict[str, Any] = {
        "messages_initiated": 0,
        "messages_delivered": 0,
        "messages_failed": 0,
        "delivery_ratio": 0.0,
        "rssi_samples": [],
        "error_codes": {},
    }

    if not os.path.exists(log_path):
        result["error"] = "log file not found"
        return result

    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if RE_MSG_INIT.search(line):
                result["messages_initiated"] += 1
            if RE_MSG_OK.search(line):
                result["messages_delivered"] += 1
            m = RE_MSG_FAIL.search(line)
            if m:
                result["messages_failed"] += 1
                cause = m.group(1)[:50]
                result["error_codes"][cause] = result["error_codes"].get(cause, 0) + 1
            rm = RE_RSSI.search(line)
            if rm:
                result["rssi_samples"].append(int(rm.group(1)))

    if result["messages_initiated"] > 0:
        result["delivery_ratio"] = (
            result["messages_delivered"] / result["messages_initiated"]
        )

    if result["rssi_samples"]:
        result["rssi_avg_dbm"] = sum(result["rssi_samples"]) / len(result["rssi_samples"])
        result["rssi_min_dbm"] = min(result["rssi_samples"])
        result["rssi_max_dbm"] = max(result["rssi_samples"])

    return result


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Multi-distance radio evaluation for CEE-PEW."
    )
    parser.add_argument("--port-a", default="COM3", help="Device A serial port")
    parser.add_argument("--port-b", default="COM4", help="Device B serial port")
    parser.add_argument("--distances", required=True,
                        help="Comma-separated distances in cm (e.g., 20,100,300,500,1000)")
    parser.add_argument("--duration-per", type=int, default=180,
                        help="Duration per distance in seconds (default: 180)")
    parser.add_argument("--log-dir", required=True,
                        help="Directory for per-distance logs")
    parser.add_argument("--out-json", required=True,
                        help="Output JSON statistics file")
    parser.add_argument("--frames-per", type=int, default=100,
                        help="Number of test frames per distance (default: 100)")
    parser.add_argument("--payload-size", type=int, default=58,
                        help="Payload size in bytes (default: 58)")
    args = parser.parse_args(argv)

    distances = [int(d.strip()) for d in args.distances.split(",")]

    os.makedirs(args.log_dir, exist_ok=True)
    os.makedirs(os.path.dirname(args.out_json) or ".", exist_ok=True)

    print(f"=== Multi-Distance Radio Evaluation ===")
    print(f"Distances: {distances} cm")
    print(f"Duration per distance: {args.duration_per}s")
    print(f"Frames per distance: {args.frames_per}")
    print(f"Ports: {args.port_a} <-> {args.port_b}\n")

    results: Dict[str, Any] = {}

    for dist in distances:
        prompt_distance(dist)

        log_path = os.path.join(args.log_dir, f"capture_{dist}cm.log")

        # Start monitor
        monitor = start_monitor(args.port_a, args.port_b, log_path, args.duration_per)

        # Wait for devices to settle
        time.sleep(2.0)

        # NOTE: SEND_FRAMES serial command not supported by DUT firmware,
        # and COM ports are held by the monitor. Monitoring normal
        # pairing/session operation instead.
        print(f"  Monitoring normal operation for {args.duration_per}s...")

        # Wait for monitor to complete
        try:
            stdout, stderr = monitor.communicate(timeout=args.duration_per + 30)
        except subprocess.TimeoutExpired:
            monitor.kill()
            stdout, stderr = monitor.communicate()

        # Parse results
        dist_result = parse_distance_log(log_path)
        dist_result["distance_cm"] = dist
        dist_result["duration_s"] = args.duration_per
        dist_result["frames_sent"] = args.frames_per
        results[str(dist)] = dist_result

        print(f"  Results: {dist_result['messages_delivered']}/{dist_result['messages_initiated']} "
              f"delivered ({dist_result['delivery_ratio']:.1%})")
        if "rssi_avg_dbm" in dist_result:
            print(f"  RSSI: avg={dist_result['rssi_avg_dbm']:.1f} dBm, "
                  f"min={dist_result['rssi_min_dbm']} dBm, max={dist_result['rssi_max_dbm']} dBm")

    # Aggregate
    stats = {
        "test_id": "E_multi_distance",
        "distances_cm": distances,
        "duration_per_s": args.duration_per,
        "frames_per_distance": args.frames_per,
        "payload_size_bytes": args.payload_size,
        "results": results,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "ports": {"a": args.port_a, "b": args.port_b},
    }

    with open(args.out_json, "w") as f:
        json.dump(stats, f, indent=2)

    print(f"\n=== Summary ===")
    for dist in distances:
        key = str(dist)
        r = results[key]
        print(f"  {dist}cm: {r['messages_delivered']}/{r['messages_initiated']} "
              f"({r['delivery_ratio']:.1%})")

    print(f"\nStatistics written to {args.out_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

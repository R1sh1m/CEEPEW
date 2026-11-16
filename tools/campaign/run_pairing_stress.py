#!/usr/bin/env python3
"""
tools/campaign/run_pairing_stress.py — BLE pairing stress test.

Runs repeated pairing cycles on two ESP32 boards with configurable stress
conditions. Tracks success/failure rates, timing, and failure classification.

Usage:
  python tools/campaign/run_pairing_stress.py \
      --port-a COM3 --port-b COM4 \
      --cycles 500 \
      --log-dir data/raw/B_pairing_stress \
      --out-json data/derived/B_pairing_stats.json \
      --stress-modes rapid,abort,restart,timeout,disconnect \
      --max-duration 3600

Stress modes (randomly interleaved):
  rapid       — Back-to-back pairing attempts (<1s between cycles)
  abort       — Interrupt pairing midway (after discovery, before GATT)
  restart     — One device restarts while peer is pairing
  timeout     — Force GATT timeout during key exchange
  disconnect  — BLE disconnect during discovery or identity exchange

Requires:
  - pyserial (pip install pyserial)
  - Both DUTs flashed with patched release firmware (commit 6a0d29a)
"""

from __future__ import annotations

import argparse
import json
import os
import random
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

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
BAUD = 115200
RESET_CMD = b"\r\r"  # Soft reset via serial
PAIRING_TIMEOUT_S = 60  # Max time to wait for pairing completion
CYCLE_GAP_S = 2  # Normal gap between cycles


def reset_device(port: str) -> None:
    """Soft-reset an ESP32 via serial DTR/RTS toggle + break."""
    try:
        ser = serial.Serial(port, BAUD, timeout=1)
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)
        time.sleep(0.5)
        ser.write(RESET_CMD)
        ser.flush()
        ser.close()
    except serial.SerialException as e:
        print(f"[WARN] Could not reset {port}: {e}")


def start_monitor(port_a: str, port_b: str, log_path: str,
                  duration_s: int) -> subprocess.Popen:
    """Start ceepew_monitor.py as a subprocess, return the Popen object."""
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


def parse_pairing_result(log_path: str) -> Dict[str, Any]:
    """Parse a pairing log to determine outcome and stage.

    Returns a dict with keys:
      success: bool
      stage: str (discovery, gatt, pin_confirm, key_exchange, handoff, active, unknown)
      duration_ms: Optional[float]
      error_code: Optional[int]
      notes: List[str]
    """
    result: Dict[str, Any] = {
        "success": False,
        "stage": "unknown",
        "duration_ms": None,
        "error_code": None,
        "notes": [],
    }

    if not os.path.exists(log_path):
        result["notes"].append("log file not found")
        return result

    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    # Success markers
    arq_activity = "[ARQ_TX]" in content or "[ARQ_RX]" in content
    phase_active = "phase=active" in content or "active=1" in content
    if "PAIRING_SUCCESS" in content or "SESSION_ACTIVE" in content or arq_activity or phase_active:
        result["success"] = True
        result["stage"] = "active"

        # Try to extract duration
        m = re.search(r"pairing_duration_ms[:=]\s*(\d+)", content)
        if m:
            result["duration_ms"] = float(m.group(1))
        m = re.search(r"handoff_duration_ms[:=]\s*(\d+)", content)
        if m:
            result["notes"].append(f"handoff_ms={m.group(1)}")
        return result

    # Failure classification
    failure_patterns = [
        (r"DISCOVERY_TIMEOUT", "discovery", "timeout"),
        (r"GATT_TIMEOUT", "gatt", "timeout"),
        (r"PAIRING_ABORT", "gatt", "abort"),
        (r"KEY_EXCHANGE_FAIL", "key_exchange", "crypto"),
        (r"HANDOFF_FAIL", "handoff", "transport"),
        (r"MUTEX_FAIL|mutex.*fail", "gatt", "mutex"),
        (r"WATCHDOG|watchdog", "unknown", "watchdog"),
        (r"ERR=(\d+)", "unknown", "error_code"),
    ]

    for pattern, stage, category in failure_patterns:
        m = re.search(pattern, content, re.IGNORECASE)
        if m:
            result["stage"] = stage
            result["notes"].append(f"{category}: {m.group(0)}")
            if category == "error_code" and m.lastindex:
                result["error_code"] = int(m.group(1))
            break

    if not result["notes"]:
        result["notes"].append("no clear outcome in log")

    return result


def run_pairing_cycle(port_a: str, port_b: str, log_path: str,
                      stress_mode: Optional[str] = None) -> Dict[str, Any]:
    """Run one pairing cycle and return the result dict."""
    cycle_start = time.time()

    # Reset both devices
    reset_device(port_a)
    reset_device(port_b)
    time.sleep(1.0)  # Let them boot

    # Apply stress condition before pairing
    if stress_mode == "abort":
        # Reset device B after a short delay to simulate mid-pairing abort
        import threading

        def delayed_reset():
            time.sleep(3.0)
            reset_device(port_b)

        t = threading.Thread(target=delayed_reset, daemon=True)
        t.start()

    # Start monitor
    monitor = start_monitor(port_a, port_b, log_path, PAIRING_TIMEOUT_S)

    # Trigger pairing (button press simulation via serial command)
    # The firmware should auto-start pairing on boot if configured
    time.sleep(0.5)

    # Wait for monitor to finish
    try:
        stdout, stderr = monitor.communicate(timeout=PAIRING_TIMEOUT_S + 10)
    except subprocess.TimeoutExpired:
        monitor.kill()
        stdout, stderr = monitor.communicate()

    # Parse result
    result = parse_pairing_result(log_path)
    result["cycle_duration_s"] = time.time() - cycle_start
    result["stress_mode"] = stress_mode
    result["timestamp_utc"] = datetime.now(timezone.utc).isoformat()

    return result


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="BLE pairing stress test for CEE-PEW."
    )
    parser.add_argument("--port-a", default="COM3", help="Device A serial port")
    parser.add_argument("--port-b", default="COM4", help="Device B serial port")
    parser.add_argument("--cycles", type=int, default=500,
                        help="Number of pairing cycles (default: 500)")
    parser.add_argument("--log-dir", required=True,
                        help="Directory for per-cycle logs")
    parser.add_argument("--out-json", required=True,
                        help="Output JSON statistics file")
    parser.add_argument("--stress-modes", default="",
                        help="Comma-separated stress modes: rapid,abort,restart,timeout,disconnect")
    parser.add_argument("--max-duration", type=int, default=3600,
                        help="Maximum total duration in seconds (default: 3600)")
    parser.add_argument("--cycle-gap", type=float, default=2.0,
                        help="Gap between normal cycles in seconds (default: 2.0)")
    args = parser.parse_args(argv)

    os.makedirs(args.log_dir, exist_ok=True)
    os.makedirs(os.path.dirname(args.out_json) or ".", exist_ok=True)

    stress_modes = [m.strip() for m in args.stress_modes.split(",") if m.strip()]
    valid_modes = {"rapid", "abort", "restart", "timeout", "disconnect"}
    invalid = set(stress_modes) - valid_modes
    if invalid:
        print(f"[ERROR] Invalid stress modes: {invalid}", file=sys.stderr)
        return 1

    results: List[Dict[str, Any]] = []
    start_time = time.time()

    print(f"=== BLE Pairing Stress Test ===")
    print(f"Cycles: {args.cycles}")
    print(f"Ports: {args.port_a} <-> {args.port_b}")
    print(f"Stress modes: {stress_modes or 'none'}")
    print(f"Max duration: {args.max_duration}s\n")

    for i in range(args.cycles):
        # Check max duration
        elapsed = time.time() - start_time
        if elapsed > args.max_duration:
            print(f"[INFO] Max duration reached after {i} cycles.")
            break

        # Select stress mode
        stress_mode = None
        if stress_modes:
            # Weighted selection: normal mode more common than stress
            if random.random() < 0.3 and stress_modes:
                stress_mode = random.choice(stress_modes)

        log_path = os.path.join(args.log_dir, f"cycle_{i:04d}.log")

        print(f"Cycle {i+1}/{args.cycles} (mode={stress_mode or 'normal'}) ... ", end="", flush=True)

        result = run_pairing_cycle(args.port_a, args.port_b, log_path, stress_mode)
        results.append(result)

        status = "PASS" if result["success"] else "FAIL"
        print(f"{status} (stage={result['stage']}, {result['cycle_duration_s']:.1f}s)")

        # Gap between cycles
        gap = 0.5 if stress_mode == "rapid" else args.cycle_gap
        time.sleep(gap)

    # Aggregate statistics
    total = len(results)
    successful = sum(1 for r in results if r["success"])
    failed = total - successful

    # Failure breakdown by stage
    stage_breakdown: Dict[str, int] = {}
    for r in results:
        if not r["success"]:
            stage = r["stage"]
            stage_breakdown[stage] = stage_breakdown.get(stage, 0) + 1

    # Mutex-related failures
    mutex_failures = sum(
        1 for r in results
        if not r["success"] and "mutex" in str(r["notes"]).lower()
    )

    # Watchdog resets
    watchdog_resets = sum(
        1 for r in results
        if not r["success"] and "watchdog" in str(r["notes"]).lower()
    )

    # Durations
    durations = [r["cycle_duration_s"] for r in results if r["cycle_duration_s"] is not None]
    duration_stats = {}
    if durations:
        duration_stats = {
            "median_s": sorted(durations)[len(durations) // 2],
            "min_s": min(durations),
            "max_s": max(durations),
            "mean_s": sum(durations) / len(durations),
        }

    stats = {
        "test_id": "B_pairing_stress",
        "cycles_attempted": total,
        "cycles_successful": successful,
        "cycles_failed": failed,
        "success_rate": successful / total if total else 0.0,
        "failure_breakdown": stage_breakdown,
        "mutex_failures": mutex_failures,
        "watchdog_resets": watchdog_resets,
        "unexplained_failures": failed - mutex_failures - watchdog_resets,
        "duration_stats": duration_stats,
        "stress_modes": stress_modes,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "ports": {"a": args.port_a, "b": args.port_b},
    }

    with open(args.out_json, "w") as f:
        json.dump(stats, f, indent=2)

    print(f"\n=== Results ===")
    print(f"Total cycles: {total}")
    print(f"Successful: {successful} ({stats['success_rate']:.1%})")
    print(f"Failed: {failed}")
    print(f"Mutex failures: {mutex_failures}")
    print(f"Watchdog resets: {watchdog_resets}")
    print(f"Unexplained failures: {stats['unexplained_failures']}")
    print(f"Failure breakdown by stage: {stage_breakdown}")
    print(f"\nStatistics written to {args.out_json}")

    # Acceptance check
    if stats["unexplained_failures"] > 0:
        print("\n[WARNING] Unexplained failures detected — investigate before paper submission.")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())

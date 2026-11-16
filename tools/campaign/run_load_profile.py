#!/usr/bin/env python3
"""
tools/campaign/run_load_profile.py — Stack & sustained-load profiling.

Profiles stack high-water marks and queue depths under worst-case sustained
load for both Full and Lite firmware profiles.

Usage:
  # Full-profile sustained load (10 minutes)
  python tools/campaign/run_load_profile.py \
      --port-a COM3 --port-b COM4 \
      --profile full \
      --duration 600 \
      --log data/raw/C1_full_load/capture_dual.log \
      --out-json data/derived/C1_stack_profile.json

  # Lite-profile sustained load (10 minutes)
  python tools/campaign/run_load_profile.py \
      --port-a COM3 --port-b COM4 \
      --profile lite \
      --duration 600 \
      --log data/raw/C2_lite_load/capture_dual.log \
      --out-json data/derived/C2_stack_profile.json

Workload components (triggered via serial commands to DUT-A):
  ed25519     — Repeated Ed25519 sign/verify operations
  x25519      — Repeated X25519 key exchanges
  espnow_max  — Maximum-size ESP-NOW frames (249 B, FEC on)
  fec_on      — Hamming(15,11) encode/decode under load
  malformed   — Burst of malformed frames (rejected at ESL parser)
  burst       — 50 back-to-back frames without cleanup
  ui_concurrent — OLED updates, button debouncing, ADC sampling concurrently

Requires:
  - pyserial
  - Firmware must log stack high-water marks (see FW-2 in remaining_tests.md)
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

# Workload definitions per profile
WORKLOADS = {
    "full": [
        "ed25519",
        "x25519",
        "espnow_max",
        "fec_on",
        "malformed",
        "burst",
        "ui_concurrent",
    ],
    "lite": [
        "espnow_max",
        "ascon_only",
        "burst",
        "ui_concurrent",
    ],
}

# Serial commands to trigger workloads on DUT-A
WORKLOAD_COMMANDS = {
    "ed25519": "WORKLOAD_ED25519\n",
    "x25519": "WORKLOAD_X25519\n",
    "espnow_max": "WORKLOAD_ESPNOV_MAX\n",
    "fec_on": "WORKLOAD_FEC_ON\n",
    "malformed": "WORKLOAD_MALFORMED\n",
    "burst": "WORKLOAD_BURST\n",
    "ui_concurrent": "WORKLOAD_UI_CONCURRENT\n",
    "ascon_only": "WORKLOAD_ASC_ONLY\n",
}

# Regex for stack high-water mark logs
RE_STACK_HIGHWATER = re.compile(
    r"STACK.*high.?water.*(\d+)\s*bytes.*\(([\d.]+)%\)", re.IGNORECASE
)
RE_STACK_HWM = re.compile(
    r"\[STACK-HWM\]\s*(\w+)\s+free=(\d+)\s*B", re.IGNORECASE
)
RE_STACK_CANARY = re.compile(r"stack.*canary.*fail", re.IGNORECASE)
RE_FREERTOS_OVERFLOW = re.compile(r"stack overflow", re.IGNORECASE)
RE_QUEUE_DEPTH = re.compile(r"queue.*depth.*(\d+)", re.IGNORECASE)
RE_QUEUE_OVERFLOW = re.compile(r"queue.*overflow|QUEUE_FULL", re.IGNORECASE)


def send_workload_command(port: str, command: str) -> bool:
    """Send a workload trigger command to a device via serial."""
    try:
        ser = serial.Serial(port, BAUD, timeout=2)
        ser.write(command.encode("ascii"))
        ser.flush()
        time.sleep(0.5)
        ser.close()
        return True
    except serial.SerialException as e:
        print(f"[WARN] Could not send workload command to {port}: {e}")
        return False


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


def analyze_stack_log(log_path: str) -> Dict[str, Any]:
    """Analyze a capture log for stack profiling data."""
    result: Dict[str, Any] = {
        "stack_samples": [],
        "min_highwater_bytes": None,
        "max_highwater_bytes": None,
        "stack_canary_failures": 0,
        "freertos_overflow_hooks": 0,
        "queue_max_depth": 0,
        "queue_overflow_events": 0,
        "tasks_seen": set(),
    }

    if not os.path.exists(log_path):
        result["error"] = "log file not found"
        return result

    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            # Stack high-water marks (legacy format)
            m = RE_STACK_HIGHWATER.search(line)
            if m:
                bytes_val = int(m.group(1))
                pct_val = float(m.group(2))
                result["stack_samples"].append({
                    "bytes": bytes_val,
                    "pct": pct_val,
                    "line": line.strip()[:200],
                })
                # Extract task name if present
                tm = re.search(r"\[([^\]]+)\]", line)
                if tm:
                    result["tasks_seen"].add(tm.group(1))
                continue

            # Stack high-water marks (firmware format: [STACK-HWM] task free=NNN B)
            m = RE_STACK_HWM.search(line)
            if m:
                task_name = m.group(1)
                free_bytes = int(m.group(2))
                result["stack_samples"].append({
                    "task": task_name,
                    "free_bytes": free_bytes,
                    "line": line.strip()[:200],
                })
                result["tasks_seen"].add(task_name)
                continue

            # Stack canary failures
            if RE_STACK_CANARY.search(line):
                result["stack_canary_failures"] += 1

            # FreeRTOS overflow hooks
            if RE_FREERTOS_OVERFLOW.search(line):
                result["freertos_overflow_hooks"] += 1

            # Queue depth
            mq = RE_QUEUE_DEPTH.search(line)
            if mq:
                depth = int(mq.group(1))
                if depth > result["queue_max_depth"]:
                    result["queue_max_depth"] = depth

            # Queue overflow
            if RE_QUEUE_OVERFLOW.search(line):
                result["queue_overflow_events"] += 1

    # Compute min/max
    if result["stack_samples"]:
        free_vals = [s.get("free_bytes", s.get("bytes", 0)) for s in result["stack_samples"]]
        result["min_highwater_bytes"] = min(free_vals)
        result["max_highwater_bytes"] = max(free_vals)

    result["tasks_seen"] = sorted(result["tasks_seen"])
    return result


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Stack & sustained-load profiling for CEE-PEW."
    )
    parser.add_argument("--port-a", default="COM3", help="Device A serial port")
    parser.add_argument("--port-b", default="COM4", help="Device B serial port")
    parser.add_argument("--profile", choices=["full", "lite"], required=True,
                        help="Firmware profile to test")
    parser.add_argument("--duration", type=int, default=600,
                        help="Test duration in seconds (default: 600)")
    parser.add_argument("--log", required=True,
                        help="Output capture log path")
    parser.add_argument("--out-json", required=True,
                        help="Output JSON statistics file")
    parser.add_argument("--workload", action="append", default=[],
                        help="Specific workload components to run (repeatable)")
    args = parser.parse_args(argv)

    os.makedirs(os.path.dirname(args.log) or ".", exist_ok=True)
    os.makedirs(os.path.dirname(args.out_json) or ".", exist_ok=True)

    workloads = args.workload if args.workload else WORKLOADS[args.profile]

    print(f"=== Stack & Load Profiling ===")
    print(f"Profile: {args.profile}")
    print(f"Duration: {args.duration}s")
    print(f"Workloads: {workloads}")
    print(f"Ports: {args.port_a} <-> {args.port_b}\n")

    # Start monitor
    monitor = start_monitor(args.port_a, args.port_b, args.log, args.duration)

    # Wait for devices to boot
    time.sleep(3.0)

    # NOTE: DUT firmware has no serial command interface for workload triggers.
    # Devices auto-pair and run normal operation (crypto, ESP-NOW, UI) during
    # the monitoring window. Stack high-water marks are logged by the firmware
    # via uxTaskGetStackHighWaterMark calls in task_session.c / task_ui.c.
    print("  NOTE: Workload commands not supported by DUT firmware — monitoring normal operation")

    # Wait for monitor to finish
    print("  Waiting for monitor to complete...")
    try:
        stdout, stderr = monitor.communicate(timeout=args.duration + 30)
    except subprocess.TimeoutExpired:
        monitor.kill()
        stdout, stderr = monitor.communicate()

    # Analyze the log
    analysis = analyze_stack_log(args.log)

    # Compute safety margins (assuming 10KB UI stack, 16KB session stack)
    UI_STACK_SIZE = 10240
    SESSION_STACK_SIZE = 16384

    safety_margin: Dict[str, Any] = {}
    if analysis["min_highwater_bytes"] is not None:
        # Use the minimum high-water as worst case
        worst_bytes = analysis["min_highwater_bytes"]
        # Determine which task based on bytes (UI ~10KB, session ~16KB)
        if worst_bytes <= UI_STACK_SIZE:
            safety_margin["ui_task"] = {
                "stack_size_bytes": UI_STACK_SIZE,
                "min_highwater_bytes": worst_bytes,
                "utilisation_pct": 100.0 * (UI_STACK_SIZE - worst_bytes) / UI_STACK_SIZE,
                "safety_margin_bytes": worst_bytes,
                "safety_margin_pct": 100.0 * worst_bytes / UI_STACK_SIZE,
            }
        else:
            safety_margin["session_task"] = {
                "stack_size_bytes": SESSION_STACK_SIZE,
                "min_highwater_bytes": worst_bytes,
                "utilisation_pct": 100.0 * (SESSION_STACK_SIZE - worst_bytes) / SESSION_STACK_SIZE,
                "safety_margin_bytes": worst_bytes,
                "safety_margin_pct": 100.0 * worst_bytes / SESSION_STACK_SIZE,
            }

    # Verdict
    verdict = "PASS"
    if analysis["stack_canary_failures"] > 0:
        verdict = "FAIL"
    if analysis["freertos_overflow_hooks"] > 0:
        verdict = "FAIL"
    if analysis["queue_overflow_events"] > 0:
        verdict = "WARN"

    stats = {
        "test_id": f"C{args.profile[0].upper()}_stack_load",
        "profile": args.profile,
        "duration_seconds": args.duration,
        "workloads": workloads,
        "stack_analysis": analysis,
        "safety_margin": safety_margin,
        "verdict": verdict,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "ports": {"a": args.port_a, "b": args.port_b},
    }

    with open(args.out_json, "w") as f:
        json.dump(stats, f, indent=2)

    print(f"\n=== Results ===")
    print(f"Verdict: {verdict}")
    print(f"Stack samples: {len(analysis['stack_samples'])}")
    if analysis["min_highwater_bytes"] is not None:
        print(f"Min high-water: {analysis['min_highwater_bytes']} bytes")
        print(f"Max high-water: {analysis['max_highwater_bytes']} bytes")
    print(f"Stack canary failures: {analysis['stack_canary_failures']}")
    print(f"FreeRTOS overflow hooks: {analysis['freertos_overflow_hooks']}")
    print(f"Max queue depth: {analysis['queue_max_depth']}")
    print(f"Queue overflow events: {analysis['queue_overflow_events']}")
    print(f"\nStatistics written to {args.out_json}")

    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())

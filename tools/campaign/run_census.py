#!/usr/bin/env python3
"""tools/campaign/run_census.py - census of the hardware run archive.

The manuscript used to state run, session and handoff counts that no
artifact supported. This script computes them from the archived logs so
that every count in the paper can be traced to a file and a line pattern.

Counted, with the line pattern that defines each count:

  runs        one capture directory under data/raw/ that contains at least
              one capture_DEVICE_*.log, plus one file per logs/pairing_*.txt
  sessions    transitions of the periodic status line into phase=active,
              counted per device; a session needs the transition on both
              sides, so the pair count is the smaller of the two per-device
              counts
  handoffs    "HANDOFF_READY sync complete - both peers ready"
  pfs_rekeys  "PFS handshake complete - session key rotated"

Usage:
    python tools/campaign/run_census.py
    python tools/campaign/run_census.py --out-json data/derived/census.json
    python tools/campaign/run_census.py --runs data/raw/E3_replay_20261019
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Any, Dict, List, Optional

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
RAW_DIR = os.path.join(PROJECT_ROOT, "data", "raw")
LOG_DIR = os.path.join(PROJECT_ROOT, "logs")

STATUS_RE = re.compile(r"\[(?P<ms>\d+) ms\]\s+phase=(?P<phase>[a-z_]+)")
HANDOFF_RE = re.compile(r"HANDOFF_READY sync complete")
HANDOFF_SENT_RE = re.compile(r"Sent HANDOFF_READY beacon")
PFS_RE = re.compile(r"PFS handshake complete")
PFS_KEYPAIR_RE = re.compile(r"PFS keypair generated")
DEVICE_RE = re.compile(r"\[(DEVICE_[A-Z0-9]+)\]")


def device_logs_in(directory: str) -> List[str]:
    names = []
    for name in sorted(os.listdir(directory)):
        if name.startswith("capture_DEVICE_") and name.endswith(".log"):
            names.append(os.path.join(directory, name))
    return names


def parse_device_log(path: str) -> Dict[str, Any]:
    """Count the session, handoff and PFS events in one device log."""
    result: Dict[str, Any] = {
        "path": os.path.relpath(path, PROJECT_ROOT).replace("\\", "/"),
        "sessions": 0,
        "handoffs_completed": 0,
        "handoffs_announced": 0,
        "pfs_rekeys": 0,
        "pfs_keypairs": 0,
        "status_lines": 0,
    }
    last_phase = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = STATUS_RE.search(line)
            if m:
                result["status_lines"] += 1
                phase = m.group("phase")
                if phase == "active" and last_phase != "active":
                    result["sessions"] += 1
                last_phase = phase
                continue
            if HANDOFF_RE.search(line):
                result["handoffs_completed"] += 1
            elif HANDOFF_SENT_RE.search(line):
                result["handoffs_announced"] += 1
            if PFS_RE.search(line):
                result["pfs_rekeys"] += 1
            elif PFS_KEYPAIR_RE.search(line):
                result["pfs_keypairs"] += 1
    return result


def collect(only_runs: Optional[List[str]] = None) -> Dict[str, Any]:
    """Census every archived capture run."""
    runs: List[Dict[str, Any]] = []

    run_dirs: List[str] = []
    if os.path.isdir(RAW_DIR):
        for name in sorted(os.listdir(RAW_DIR)):
            path = os.path.join(RAW_DIR, name)
            if os.path.isdir(path):
                run_dirs.append(path)
    if only_runs:
        wanted = {os.path.basename(r.rstrip("/\\")) for r in only_runs}
        run_dirs = [d for d in run_dirs if os.path.basename(d) in wanted]

    for directory in run_dirs:
        logs = device_logs_in(directory)
        if not logs:
            continue
        per_log = [parse_device_log(path) for path in logs]
        devices = {}
        for record in per_log:
            name = os.path.basename(record["path"]).replace("capture_", "").replace(".log", "")
            devices[name] = record
        session_counts = [r["sessions"] for r in per_log]
        runs.append(
            {
                "run": os.path.basename(directory),
                "path": os.path.relpath(directory, PROJECT_ROOT).replace("\\", "/"),
                "device_logs": len(per_log),
                "sessions_by_device": {os.path.basename(r["path"]): r["sessions"] for r in per_log},
                "session_pairs": min(session_counts) if session_counts else 0,
                "handoffs_by_device": {os.path.basename(r["path"]): r["handoffs_completed"] for r in per_log},
                "pfs_rekeys_by_device": {os.path.basename(r["path"]): r["pfs_rekeys"] for r in per_log},
                "logs": per_log,
            }
        )

    pairing_logs = []
    if os.path.isdir(LOG_DIR):
        for name in sorted(os.listdir(LOG_DIR)):
            if name.startswith("pairing_") and name.endswith(".txt"):
                pairing_logs.append(os.path.join(LOG_DIR, name))

    total_sessions_by_device: Dict[str, int] = {}
    total_handoffs = 0
    total_pfs = 0
    for run in runs:
        for log in run["logs"]:
            name = os.path.basename(log["path"]).replace("capture_", "").replace(".log", "")
            total_sessions_by_device[name] = total_sessions_by_device.get(name, 0) + log["sessions"]
        total_handoffs += sum(log["handoffs_completed"] for log in run["logs"])
        total_pfs += sum(log["pfs_rekeys"] for log in run["logs"])

    device_session_counts = sorted(total_sessions_by_device.values())
    session_pairs_total = min(device_session_counts) if device_session_counts else 0

    return {
        "capture_runs": len(runs),
        "pairing_logs": len(pairing_logs),
        "runs": runs,
        "pairing_log_paths": [
            os.path.relpath(p, PROJECT_ROOT).replace("\\", "/") for p in pairing_logs
        ],
        "totals": {
            "sessions_by_device": total_sessions_by_device,
            "session_pairs": session_pairs_total,
            "handoffs_completed_total": total_handoffs,
            "pfs_rekeys_total": total_pfs,
        },
    }


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Census the archived hardware runs.")
    parser.add_argument("--out-json", default="", help="write the census as JSON")
    parser.add_argument("--runs", nargs="*", default=None, help="limit the census to these run directories")
    args = parser.parse_args(argv)

    census = collect(args.runs)

    print("capture runs      : %d" % census["capture_runs"])
    print("pairing logs      : %d" % census["pairing_logs"])
    totals = census["totals"]
    print("sessions by device: %s" % totals["sessions_by_device"])
    print("session pairs     : %d" % totals["session_pairs"])
    print("handoffs completed: %d" % totals["handoffs_completed_total"])
    print("PFS re-keys       : %d" % totals["pfs_rekeys_total"])
    print("")
    print("%-34s %8s %10s %10s" % ("run", "sessions", "handoffs", "pfs_rekeys"))
    for run in census["runs"]:
        handoffs = sum(run["handoffs_by_device"].values())
        rekeys = sum(run["pfs_rekeys_by_device"].values())
        print("%-34s %8d %10d %10d" % (run["run"], run["session_pairs"], handoffs, rekeys))

    if args.out_json:
        os.makedirs(os.path.dirname(os.path.abspath(args.out_json)), exist_ok=True)
        with open(args.out_json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(census, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("wrote %s" % args.out_json)

    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
tools/campaign/run_fec_analysis.py — FEC correction statistics.

Analyzes FEC-controlled corruption test logs to compute correction/detection
rates for Hamming(15,11) under various error patterns.

Usage:
  python tools/campaign/run_fec_analysis.py \
      --log-dir data/raw/D_fec_corruption \
      --out-json data/derived/D_fec_results.json

Expected log files (one per error type):
  D_clean.log      — Baseline (no corruption)
  D_1bit.log       — 1-bit errors injected
  D_2bit.log       — 2-bit errors injected
  D_3bit.log       — 3-bit errors injected
  D_burst.log      — Burst errors injected

The script looks for firmware log markers:
  [SECURE_CHAT_RX] Hamming(15,11) FEC corrected single-bit error   (correction)
  [SECURE_CHAT_RX] Hamming decode failed                           (detection)
  [SECURE_CHAT_RX] CRC verified (FEC off)                          (FEC off path)
  [SECURE_CHAT_RX] Ascon-128 AEAD auth tag verification FAILED     (crypto reject)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from datetime import datetime, timezone
from typing import Optional, List, Dict, Any

PROJECT_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)

# Regex patterns for FEC events
RE_FEC_CORRECT = re.compile(
    r"Hamming\(15,11\)\s+FEC\s+corrected\s+single-bit\s+error", re.IGNORECASE
)
RE_FEC_DECODE_FAIL = re.compile(
    r"Hamming\s+decode\s+failed", re.IGNORECASE
)
RE_FEC_OFF_RX = re.compile(
    r"CRC\s+verified\s*\(FEC\s+off\)", re.IGNORECASE
)
RE_ASCON_FAIL = re.compile(
    r"Ascon-128\s+AEAD\s+auth\s+tag\s+verification\s+FAILED", re.IGNORECASE
)
RE_MSG_DELIVERED = re.compile(
    r"All\s+(\d+)\s+fragments\s+successfully\s+delivered", re.IGNORECASE
)
RE_MSG_FAIL = re.compile(
    r"FAILED:\s*(.+)$", re.IGNORECASE
)


def analyze_fec_log(log_path: str) -> Dict[str, Any]:
    """Analyze a single FEC test log."""
    result: Dict[str, Any] = {
        "fec_corrections": 0,
        "fec_decode_failures": 0,
        "fec_off_rx": 0,
        "ascon_failures": 0,
        "messages_delivered": 0,
        "messages_failed": 0,
        "error_codes": {},
    }

    if not os.path.exists(log_path):
        result["error"] = "log file not found"
        return result

    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if RE_FEC_CORRECT.search(line):
                result["fec_corrections"] += 1
            if RE_FEC_DECODE_FAIL.search(line):
                result["fec_decode_failures"] += 1
            if RE_FEC_OFF_RX.search(line):
                result["fec_off_rx"] += 1
            if RE_ASCON_FAIL.search(line):
                result["ascon_failures"] += 1
            m = RE_MSG_DELIVERED.search(line)
            if m:
                result["messages_delivered"] += int(m.group(1))
            m = RE_MSG_FAIL.search(line)
            if m:
                result["messages_failed"] += 1
                cause = m.group(1)[:60]
                result["error_codes"][cause] = result["error_codes"].get(cause, 0) + 1

    return result


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="FEC correction statistics analysis for CEE-PEW."
    )
    parser.add_argument("--log-dir", required=True,
                        help="Directory containing FEC test logs")
    parser.add_argument("--out-json", required=True,
                        help="Output JSON statistics file")
    parser.add_argument("--clean-log", default="D_clean.log",
                        help="Baseline log filename (default: D_clean.log)")
    parser.add_argument("--1bit-log", default="D_1bit.log",
                        help="1-bit error log filename (default: D_1bit.log)")
    parser.add_argument("--2bit-log", default="D_2bit.log",
                        help="2-bit error log filename (default: D_2bit.log)")
    parser.add_argument("--3bit-log", default="D_3bit.log",
                        help="3-bit error log filename (default: D_3bit.log)")
    parser.add_argument("--burst-log", default="D_burst.log",
                        help="Burst error log filename (default: D_burst.log)")
    args = parser.parse_args(argv)

    os.makedirs(os.path.dirname(args.out_json) or ".", exist_ok=True)

    log_files = {
        "clean": args.clean_log,
        "1bit": args._1bit_log if hasattr(args, "_1bit_log") else args.__dict__.get("1bit-log", "D_1bit.log"),
        "2bit": args._2bit_log if hasattr(args, "_2bit_log") else args.__dict__.get("2bit-log", "D_2bit.log"),
        "3bit": args._3bit_log if hasattr(args, "_3bit_log") else args.__dict__.get("3bit-log", "D_3bit.log"),
        "burst": args.burst_log,
    }

    # Fix argparse dest for arguments with hyphens
    log_files = {
        "clean": args.clean_log,
        "1bit": getattr(args, "1bit_log", "D_1bit.log"),
        "2bit": getattr(args, "2bit_log", "D_2bit.log"),
        "3bit": getattr(args, "3bit_log", "D_3bit.log"),
        "burst": args.burst_log,
    }

    results: Dict[str, Any] = {}
    for label, fname in log_files.items():
        path = os.path.join(args.log_dir, fname)
        result = analyze_fec_log(path)
        result["file"] = fname
        results[label] = result

    # Compute correction/detection rates
    summary: Dict[str, Any] = {}
    for label in ["1bit", "2bit", "3bit", "burst"]:
        r = results[label]
        total_injected = r["fec_corrections"] + r["fec_decode_failures"]
        if total_injected > 0:
            summary[label] = {
                "total_events": total_injected,
                "corrected": r["fec_corrections"],
                "detected_dropped": r["fec_decode_failures"],
                "correction_rate": r["fec_corrections"] / total_injected,
                "detection_rate": r["fec_decode_failures"] / total_injected,
                "ascon_failures": r["ascon_failures"],
                "messages_delivered": r["messages_delivered"],
                "messages_failed": r["messages_failed"],
            }

    # Verdict
    verdict = "PASS"
    issues: List[str] = []

    # 1-bit errors should be corrected
    r1 = summary.get("1bit", {})
    if r1.get("correction_rate", 0) < 1.0:
        verdict = "FAIL"
        issues.append(f"1-bit correction rate {r1.get('correction_rate', 0):.1%} < 100%")

    # 2+/3-bit/burst errors should be detected (not corrected)
    for label in ["2bit", "3bit", "burst"]:
        r = summary.get(label, {})
        if r.get("correction_rate", 0) > 0:
            verdict = "FAIL"
            issues.append(f"{label} errors were incorrectly corrected ({r.get('correction_rate', 0):.1%})")
        if r.get("detection_rate", 0) < 1.0:
            verdict = "WARN"
            issues.append(f"{label} detection rate {r.get('detection_rate', 0):.1%} < 100%")

    stats = {
        "test_id": "D_fec_corruption",
        "per_log": results,
        "summary": summary,
        "verdict": verdict,
        "issues": issues,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "log_dir": args.log_dir,
    }

    with open(args.out_json, "w") as f:
        json.dump(stats, f, indent=2)

    print(f"=== FEC Analysis Results ===")
    print(f"Verdict: {verdict}")
    for label in ["clean", "1bit", "2bit", "3bit", "burst"]:
        r = results[label]
        print(f"\n  {label.upper()}:")
        print(f"    FEC corrections: {r['fec_corrections']}")
        print(f"    FEC decode failures: {r['fec_decode_failures']}")
        print(f"    FEC off RX: {r['fec_off_rx']}")
        print(f"    Ascon failures: {r['ascon_failures']}")
        print(f"    Messages delivered: {r['messages_delivered']}")
        print(f"    Messages failed: {r['messages_failed']}")

    if issues:
        print(f"\n  Issues:")
        for issue in issues:
            print(f"    - {issue}")

    print(f"\nStatistics written to {args.out_json}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())

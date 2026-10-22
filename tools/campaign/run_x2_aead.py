#!/usr/bin/env python3
"""tools/campaign/run_x2_aead.py - same-board AEAD and signature baseline (campaign X2).

Parses the BENCHCASE / BENCHMETA lines emitted by components/crypto/crypto_bench.c
and turns them into the comparison table the manuscript uses:

    Ascon-128 v1.2  vs  AES-256-GCM  vs  AES-128-CCM  vs  ChaCha20-Poly1305
    Ed25519 (project implementation)

Every case is measured on the same board, at the same CPU frequency, with the
same 60-byte plaintext and the same 12-byte nonce, and every case carries an
explicit hardware-acceleration label. A case that the firmware build does not
provide is reported as unavailable; it is never silently dropped.

Usage:
    python tools/campaign/run_x2_aead.py --log data/raw/X2/capture_DEVICE_A.log
    python tools/campaign/run_x2_aead.py --log a.log --log b.log --out-json data/derived/x2.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Any, Dict, List, Optional

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

CASE_RE = re.compile(
    r"BENCHCASE:\s+name=(?P<name>\S+)\s+available=(?P<available>\d+)\s+hw_aes=(?P<hw_aes>\d+)"
    r"(?:\s+iters=(?P<iters>\d+))?"
    r"(?:\s+median_us=(?P<median>\d+))?"
    r"(?:\s+min_us=(?P<min>\d+))?"
    r"(?:\s+max_us=(?P<max>\d+))?"
    r"(?:\s+mean_us=(?P<mean>\d+))?"
    r"(?:\s+median_cycles=(?P<cycles>\d+))?"
    r"(?:\s+cpu_hz=(?P<cpu_hz>\d+))?"
)
META_RE = re.compile(
    r"BENCHMETA:\s+iterations=(?P<iters>\d+)\s+plaintext_len=(?P<pt>\d+)"
    r"\s+cpu_hz=(?P<cpu_hz>\d+)\s+psa=(?P<psa>\d+)\s+hw_aes=(?P<hw>\d+)\s+errors=(?P<errors>\d+)"
)

BASELINE = "ascon128_encrypt_ad"


def parse_log(path: str) -> Dict[str, Any]:
    """Extract the benchmark cases and metadata from one board log."""
    cases: List[Dict[str, Any]] = []
    meta: List[Dict[str, Any]] = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = META_RE.search(line)
            if m:
                meta.append(
                    {
                        "iterations": int(m.group("iters")),
                        "plaintext_len": int(m.group("pt")),
                        "cpu_hz": int(m.group("cpu_hz")),
                        "psa": int(m.group("psa")),
                        "hw_aes": int(m.group("hw")),
                        "errors": int(m.group("errors")),
                    }
                )
                continue
            m = CASE_RE.search(line)
            if m:
                cases.append(
                    {
                        "source": os.path.relpath(path, PROJECT_ROOT).replace("\\", "/"),
                        "name": m.group("name"),
                        "available": m.group("available") == "1",
                        "hw_aes": m.group("hw_aes") == "1",
                        "iters": int(m.group("iters")) if m.group("iters") else 0,
                        "median_us": int(m.group("median")) if m.group("median") else None,
                        "min_us": int(m.group("min")) if m.group("min") else None,
                        "max_us": int(m.group("max")) if m.group("max") else None,
                        "mean_us": int(m.group("mean")) if m.group("mean") else None,
                        "median_cycles": int(m.group("cycles")) if m.group("cycles") else None,
                        "cpu_hz": int(m.group("cpu_hz")) if m.group("cpu_hz") else 0,
                    }
                )
    return {"source": os.path.relpath(path, PROJECT_ROOT).replace("\\", "/"), "cases": cases, "meta": meta}


def aggregate(per_log: List[Dict[str, Any]]) -> Dict[str, Any]:
    """Pool the per-board cases by name."""
    names: List[str] = []
    pooled: Dict[str, Dict[str, Any]] = {}
    for entry in per_log:
        for case in entry["cases"]:
            name = case["name"]
            if name not in pooled:
                pooled[name] = {
                    "name": name,
                    "available": False,
                    "hw_aes": case["hw_aes"],
                    "boards": [],
                    "median_us": [],
                }
                names.append(name)
            record = pooled[name]
            record["available"] = record["available"] or case["available"]
            record["hw_aes"] = record["hw_aes"] or case["hw_aes"]
            record["boards"].append(
                {
                    "source": case["source"],
                    "median_us": case["median_us"],
                    "min_us": case["min_us"],
                    "max_us": case["max_us"],
                    "mean_us": case["mean_us"],
                    "median_cycles": case["median_cycles"],
                    "cpu_hz": case["cpu_hz"],
                    "iters": case["iters"],
                }
            )
            if case["median_us"] is not None:
                record["median_us"].append(case["median_us"])

    for record in pooled.values():
        medians = sorted(record["median_us"])
        if medians:
            record["pooled_median_us"] = medians[len(medians) // 2]
            record["board_spread_us"] = medians[-1] - medians[0]
        else:
            record["pooled_median_us"] = None
            record["board_spread_us"] = None

    baseline = pooled.get(BASELINE, {}).get("pooled_median_us")
    for record in pooled.values():
        value = record.get("pooled_median_us")
        if baseline and value:
            record["ascon_ratio"] = round(value / baseline, 4)
            record["interpretation"] = (
                "lower is faster; ratio = case / ascon128_encrypt_ad"
            )
        else:
            record["ascon_ratio"] = None

    return {
        "cases": [pooled[name] for name in names],
        "meta": [meta for entry in per_log for meta in entry["meta"]],
        "sources": [entry["source"] for entry in per_log],
    }


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Same-board AEAD baseline analysis (campaign X2).")
    parser.add_argument("--log", action="append", default=[], help="board log (repeatable)")
    parser.add_argument("--out-json", default="", help="write the analysis as JSON")
    args = parser.parse_args(argv)

    if not args.log:
        parser.error("at least one --log is required")

    per_log = [parse_log(path) for path in args.log]
    result = aggregate(per_log)

    print("%-28s %-6s %-8s %12s %14s %10s" % ("case", "avail", "hw_aes", "median_us", "median_cycles", "vs ascon"))
    for case in result["cases"]:
        median = case.get("pooled_median_us")
        cycles = None
        for board in case["boards"]:
            if board.get("median_cycles"):
                cycles = board["median_cycles"]
                break
        ratio = case.get("ascon_ratio")
        print(
            "%-28s %-6s %-8s %12s %14s %10s"
            % (
                case["name"],
                "yes" if case["available"] else "NO",
                "yes" if case["hw_aes"] else "no",
                "-" if median is None else median,
                "-" if cycles is None else cycles,
                "-" if ratio is None else ("%.2fx" % ratio),
            )
        )
    for meta in result["meta"]:
        print(
            "meta: iterations=%d plaintext_len=%d cpu_hz=%d psa=%d hw_aes=%d errors=%d"
            % (
                meta["iterations"],
                meta["plaintext_len"],
                meta["cpu_hz"],
                meta["psa"],
                meta["hw_aes"],
                meta["errors"],
            )
        )
    if not result["cases"]:
        print("NO BENCHCASE LINES FOUND: the log carries no same-board baseline records")

    if args.out_json:
        os.makedirs(os.path.dirname(os.path.abspath(args.out_json)), exist_ok=True)
        with open(args.out_json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(result, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("wrote %s" % args.out_json)

    return 0


if __name__ == "__main__":
    sys.exit(main())

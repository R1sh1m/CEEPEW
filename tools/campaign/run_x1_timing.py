#!/usr/bin/env python3
"""tools/campaign/run_x1_timing.py - per-stage timing analysis (campaign X1).

Parses the on-device primitive benchmark log emitted by the
CONFIG_CEEPEW_BENCH_58B build:

    I (3640) task_session: [BENCHPRIM] x25519 i=0 us=147754 err=0
    I (3640) task_session: [BENCHPRIM] begin n=25 msg_len=58

and reports, per stage, the sample count, median, min, max, mean and a
cluster-bootstrap percentile interval for the median. The cluster unit is
the benchmark iteration, so the interval accounts for the within-message
correlation of repeated measurements on one board.

All interval arithmetic comes from tools/journal/stats_core.py. This script
never computes an interval itself.

Usage:
    python tools/campaign/run_x1_timing.py --log data/raw/X1/capture_DEVICE_A.log
    python tools/campaign/run_x1_timing.py --log a.log --log b.log --out-json data/derived/x1.json
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import sys
from typing import Any, Dict, List, Optional, Tuple

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tools", "journal"))

import stats_core  # noqa: E402

BENCH_RE = re.compile(
    r"\[BENCHPRIM\]\s+(?P<stage>[A-Za-z0-9_]+)\s+i=(?P<i>\d+)\s+us=(?P<us>\d+)\s+err=(?P<err>\d+)"
)
BEGIN_RE = re.compile(r"\[BENCHPRIM\]\s+begin\s+n=(?P<n>\d+)\s+msg_len=(?P<len>\d+)")
CPU_RE = re.compile(r"cpu freq:\s*(?P<hz>\d+)\s*Hz")


def parse_log(path: str) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
    """Extract benchmark samples and run-level metadata from one log."""
    samples: List[Dict[str, Any]] = []
    meta: Dict[str, Any] = {"source": os.path.relpath(path, PROJECT_ROOT).replace("\\", "/")}
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = BEGIN_RE.search(line)
            if m:
                meta["declared_n"] = int(m.group("n"))
                meta["msg_len"] = int(m.group("len"))
                continue
            m = CPU_RE.search(line)
            if m:
                meta["cpu_hz"] = int(m.group("hz"))
                continue
            m = BENCH_RE.search(line)
            if m:
                samples.append(
                    {
                        "stage": m.group("stage"),
                        "i": int(m.group("i")),
                        "us": int(m.group("us")),
                        "err": int(m.group("err")),
                        # The cluster key must be unique per board: two boards
                        # both emit i=0..n-1, so clustering on the bare
                        # iteration number would merge two different physical
                        # measurements into one cluster.
                        "cluster": "%s#%d" % (meta["source"], int(m.group("i"))),
                    }
                )
    return samples, meta


def analyse(
    samples: List[Dict[str, Any]],
    bootstraps: int = 10000,
    seed: int = 20261021,
    level: float = 0.95,
) -> Dict[str, Any]:
    """Per-stage descriptive statistics and cluster-bootstrap intervals."""
    by_stage: Dict[str, List[Dict[str, Any]]] = {}
    for sample in samples:
        by_stage.setdefault(sample["stage"], []).append(sample)

    stages: List[Dict[str, Any]] = []
    for stage in sorted(by_stage):
        rows = sorted(by_stage[stage], key=lambda r: r["i"])
        values = [float(r["us"]) for r in rows]
        clusters = [r.get("cluster", r["i"]) for r in rows]
        errors = sorted({r["err"] for r in rows})
        desc = stats_core.describe(values)
        boot = stats_core.cluster_bootstrap_ci(
            values, clusters, stat="median", B=bootstraps, level=level, seed=seed
        )
        stages.append(
            {
                "stage": stage,
                "n": len(values),
                "errors": errors,
                "median_us": desc.get("median"),
                "min_us": desc.get("min"),
                "max_us": desc.get("max"),
                "mean_us": desc.get("mean"),
                "stdev_us": desc.get("stdev"),
                "median_ci95": [boot.get("lo"), boot.get("hi")],
                "bootstrap": {"B": bootstraps, "seed": seed, "level": level},
            }
        )
    return {"stages": stages, "total_samples": len(samples)}


def flatten_rows(samples: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    return [{"stage": s["stage"], "i": s["i"], "us": s["us"], "err": s["err"]} for s in samples]


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Analyse CEE-PEW per-stage benchmark logs (campaign X1).")
    parser.add_argument("--log", action="append", default=[], help="benchmark log (repeatable)")
    parser.add_argument("--out-json", default="", help="write the full analysis as JSON")
    parser.add_argument("--out-csv", default="", help="write the flattened samples as CSV")
    parser.add_argument("--bootstraps", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=20261021)
    args = parser.parse_args(argv)

    if not args.log:
        parser.error("at least one --log is required")

    all_samples: List[Dict[str, Any]] = []
    meta: List[Dict[str, Any]] = []
    for path in args.log:
        samples, run_meta = parse_log(path)
        meta.append(run_meta)
        all_samples.extend(samples)

    result = analyse(all_samples, bootstraps=args.bootstraps, seed=args.seed)
    result["runs"] = meta

    print("stage                n    median_us     [ci95_lo, ci95_hi]      min    max   mean")
    for stage in result["stages"]:
        lo, hi = stage["median_ci95"]
        print(
            "%-18s %4d  %11.1f  [%9.1f, %9.1f] %8.1f %8.1f %8.1f"
            % (
                stage["stage"],
                stage["n"],
                stage["median_us"],
                lo if lo is not None else float("nan"),
                hi if hi is not None else float("nan"),
                stage["min_us"],
                stage["max_us"],
                stage["mean_us"],
            )
        )
    for run in meta:
        if run.get("cpu_hz"):
            print("cpu: %d Hz (%s)" % (run["cpu_hz"], run["source"]))
    if result["total_samples"] == 0:
        print("NO SAMPLES FOUND: the logs contain no [BENCHPRIM] stage records")

    if args.out_json:
        os.makedirs(os.path.dirname(os.path.abspath(args.out_json)), exist_ok=True)
        with open(args.out_json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(result, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("wrote %s" % args.out_json)

    if args.out_csv:
        os.makedirs(os.path.dirname(os.path.abspath(args.out_csv)), exist_ok=True)
        with open(args.out_csv, "w", encoding="utf-8", newline="") as fh:
            writer = csv.DictWriter(fh, fieldnames=["stage", "i", "us", "err"])
            writer.writeheader()
            writer.writerows(flatten_rows(all_samples))
        print("wrote %s" % args.out_csv)

    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""tools/bench_timing_report.py — per-stage TX/RX timing from CEE-PEW bench logs.

Parses the [BENCHTX] / [BENCHRX] / [BENCH58_*] instrumentation lines emitted by
CONFIG_CEEPEW_BENCH_58B builds and produces a per-stage timing table with N,
median, IQR and percentiles, plus an accept/reject breakdown.

IMPORTANT (evidence class): the values in these lines are measured by
`esp_timer_get_time()` around a code region, but the surrounding run had UART
logging ENABLED. UART blocking is therefore inside the measurement window for
whatever happens after the printed stage. Treat output as class `C`
(computed-from-hardware-log) and NOT as a clean no-log latency measurement.
A GPIO-toggled / no-log timing build is required for class `M-HW` latency.

Input: monitor logs from tools/ceepew_monitor.py or tools/monitor_dual.py
(both "N.N [LABEL] I (ts) tag: msg" and "ts [LABEL] tag: msg" forms).

Usage:
  python tools/bench_timing_report.py logs/pairing_bench_20261017_104256.txt
  python tools/bench_timing_report.py --glob "logs/bench58_run*.txt" --csv data/derived/bench58_stages.csv
"""

from __future__ import annotations

import argparse
import glob
import re
import statistics
import sys
from collections import Counter, defaultdict

# "[COM5] I (24093) session_send: [BENCHTX] sign_us=332308 err=0"
# optional monitor wrappers: leading elapsed + [LABEL]
PREFIX_RE = re.compile(
    r"^(?:\s*\d+(?:\.\d+)?\s+\[[^\]]+\]\s+)?"      # monitor_dual: "1 [COM5] "
    r"\[[^\]]+\]\s*"                               # "[COM5] "
    r"(?:[IWEVD]\s*\(\s*\d+\s*\)\s*)?"             # "I (24093) "
    r"(?:[A-Za-z_0-9\-]+\s*:\s*)?"                 # "session_send: "
)

MODE_RE = re.compile(r"(?:dev|port)[=_ ]?(COM\d+|\w+)?", re.I)

RE_SIGN = re.compile(r"\[BENCHTX\]\s*sign_us=(\d+)\s+err=(-?\d+)")
RE_EDV = re.compile(r"\[BENCHRX\]\s*edverify_us=(\d+)\s+err=(-?\d+)")
RE_BOX = re.compile(r"\[BENCHRX\]\s*box_us=(\d+)\s+err=(-?\d+)")
RE_ASCON = re.compile(r"\[BENCHRX\]\s*ascon_us=(\d+)\s+err=(-?\d+)")
RE_COMMIT = re.compile(r"\[BENCHRX\]\s*commit\s+seq=(\d+)\s+err=(-?\d+)")
RE_B58TX = re.compile(r"\[BENCH58_TX\]\s*seq=(\d+)\s+len=(\d+)")
RE_B58FORGE = re.compile(r"\[BENCH58_TX\]\s*seq=(\d+)\s+forged")
RE_B58DONE = re.compile(r"\[BENCH58_DONE\]\s*sent=(\d+)")
RE_FORGE = re.compile(r"\[BENCH58_FORGE\]")
RE_TAGFAIL = re.compile(r"Ascon-128 AEAD auth tag verification FAILED")
RE_MSGRX = re.compile(r"Message received")
RE_ESLFAIL = re.compile(r"Discard: ESL transport header/MAC/replay check failed")
RE_REPLCOMMIT = re.compile(r"Discard: replay commit rejected")
RE_REGIONFAIL = re.compile(r"err=9|region.*exhaust", re.I)

STAGES = [
    ("sign_us", RE_SIGN, "Ed25519 sign (TX)"),
    ("box_us", RE_BOX, "XSalsa20/HMAC box open (RX)"),
    ("ascon_us", RE_ASCON, "Ascon-128 AEAD decrypt+verify (RX)"),
    ("edverify_us", RE_EDV, "Ed25519 verify (RX)"),
]


def strip_prefix(line: str) -> str:
    return PREFIX_RE.sub("", line.strip(), count=1)


def port_of(line: str) -> str:
    m = re.search(r"\[(COM\d+)\]", line)
    return m.group(1) if m else "-"


def pct(sorted_vals: list[float], q: float) -> float:
    """Linear-interpolation percentile, q in [0,1]."""
    if not sorted_vals:
        return float("nan")
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = q * (len(sorted_vals) - 1)
    lo = int(pos)
    hi = min(lo + 1, len(sorted_vals) - 1)
    frac = pos - lo
    return sorted_vals[lo] * (1 - frac) + sorted_vals[hi] * frac


def fmt_stats(vals: list[float]) -> str:
    if not vals:
        return "        -"
    s = sorted(vals)
    return (f"n={len(s):<4d} med={statistics.median(s):>9.0f} "
            f"p25={pct(s,0.25):>9.0f} p75={pct(s,0.75):>9.0f} "
            f"p95={pct(s,0.95):>9.0f} p99={pct(s,0.99):>9.0f} "
            f"min={s[0]:>8.0f} max={s[-1]:>9.0f}")


def analyze(paths: list[str], csv_out: str | None) -> int:
    stage_vals: dict[str, list[float]] = defaultdict(list)
    stage_err: dict[str, Counter] = defaultdict(Counter)
    counts: Counter = Counter()
    per_file: dict[str, Counter] = {}
    commit_seqs: list[int] = []

    for path in paths:
        local = Counter()
        try:
            fh = open(path, "r", encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"[FAIL] {path}: {exc}", file=sys.stderr)
            return 1
        with fh:
            for raw in fh:
                line = strip_prefix(raw)
                if not line:
                    continue
                for count_re, key in (
                    (RE_TAGFAIL, "ascon_tag_fail"), (RE_MSGRX, "app_msg_received"),
                    (RE_ESLFAIL, "esl_stage_discard"),
                    (RE_REPLCOMMIT, "replay_commit_discard"),
                    (RE_FORGE, "bench_forge_armed"),
                    (RE_REGIONFAIL, "region_err9"),
                ):
                    if count_re.search(line):
                        counts[key] += 1
                        local[key] += 1
                m = RE_B58TX.search(line)
                if m:
                    counts["b58_tx"] += 1
                    local["b58_tx"] += 1
                    continue
                m = RE_B58FORGE.search(line)
                if m:
                    counts["b58_tx_forged"] += 1
                    local["b58_tx_forged"] += 1
                    continue
                m = RE_B58DONE.search(line)
                if m:
                    counts["b58_done"] += 1
                    local["b58_done"] += 1
                    counts["b58_sent_total"] += int(m.group(1))
                    continue
                m = RE_COMMIT.search(line)
                if m:
                    commit_seqs.append(int(m.group(1)))
                    counts["commit"] += 1
                    local["commit"] += 1
                    continue
                for name, rex, _label in STAGES:
                    m = rex.search(line)
                    if m:
                        stage_vals[name].append(float(m.group(1)))
                        stage_err[name][int(m.group(2))] += 1
                        local[f"stage_{name}"] += 1
                        break
        per_file[path] = local

    print("=" * 96)
    print("CEE-PEW BENCH STAGE TIMING REPORT")
    print("=" * 96)
    print(f"files: {len(paths)}")
    for p in paths:
        print(f"  - {p}  ({sum(per_file[p].values())} recognised records)")
    print()
    print("-- per-stage timings (microseconds, as printed on-device) --")
    print("   NOTE: run had UART logging ENABLED -> class 'C', not clean no-log latency")
    for name, _rex, label in STAGES:
        vals = stage_vals[name]
        errs = dict(stage_err[name])
        print(f"  {label:<34} {fmt_stats(vals)}")
        if errs:
            print(f"  {'':<34} err histogram: {errs}")
    print()
    print("-- flow counters --")
    for k in sorted(counts):
        print(f"  {k:<28} {counts[k]}")
    if commit_seqs:
        seqs = sorted(commit_seqs)
        print(f"  commit seq range             {seqs[0]}..{seqs[-1]}"
              f"  (unique={len(set(seqs))}, dup={len(seqs)-len(set(seqs))})")
    print()

    # Verdict lines that matter for the security claims.
    # A forged frame can be rejected at ANY authenticating stage, not only Ascon:
    #   box_us     err != 0  -> outer XSalsa20/HMAC box tag rejected (outermost)
    #   ascon_us   err != 0  -> inner Ascon-128 AEAD tag rejected
    #   edverify   err != 0  -> Ed25519 signature rejected
    # The bench forge path corrupts bytes INSIDE the box ciphertext, so the
    # outermost tag catches it first and the inner stages never run.
    if counts["bench_forge_armed"]:
        armed = counts["bench_forge_armed"]
        stage_rejects = {
            "box": sum(c for e, c in stage_err["box_us"].items() if e != 0),
            "ascon": sum(c for e, c in stage_err["ascon_us"].items() if e != 0),
            "edverify": sum(c for e, c in stage_err["edverify_us"].items() if e != 0),
        }
        total_rej = sum(stage_rejects.values())
        print(f"  forged frames armed on TX      : {armed}")
        for stage, n_rej in stage_rejects.items():
            print(f"  RX rejections at {stage:<8} stage  : {n_rej}")
        print(f"  total RX authenticator rejects : {total_rej}")
        if total_rej >= armed:
            print(f"  => all {armed} armed forgeries were rejected over the air "
                  f"(outermost authenticating stage first).")
        else:
            print(f"  => only {total_rej}/{armed} armed forgeries were seen rejected "
                  f"— investigate drops (see esl_stage_discard / region_err9).")
        print(f"  RX frames reaching app layer   : {counts['app_msg_received']} "
              f"(must NOT include any forged frame)")
    if counts["region_err9"]:
        print(f"  region-allocation failures: {counts['region_err9']} (see err=9 notes)")

    if csv_out:
        import csv as _csv
        rows = []
        for name, _rex, label in STAGES:
            s = sorted(stage_vals[name])
            if not s:
                continue
            rows.append({
                "stage": name, "label": label, "n": len(s),
                "median_us": round(statistics.median(s), 1),
                "p25_us": round(pct(s, 0.25), 1), "p75_us": round(pct(s, 0.75), 1),
                "p95_us": round(pct(s, 0.95), 1), "p99_us": round(pct(s, 0.99), 1),
                "min_us": s[0], "max_us": s[-1],
                "err_histogram": ";".join(f"err{e}:{c}" for e, c in
                                          sorted(stage_err[name].items())),
            })
        with open(csv_out, "w", newline="", encoding="utf-8") as fh:
            w = _csv.DictWriter(fh, fieldnames=list(rows[0].keys()) if rows else
                                ["stage", "label", "n"])
            w.writeheader()
            w.writerows(rows)
        print(f"\nwrote {csv_out}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("logs", nargs="*", help="bench log file(s)")
    ap.add_argument("--glob", help="glob pattern for log files")
    ap.add_argument("--csv", help="write per-stage stats table to this CSV")
    args = ap.parse_args(argv)

    paths = list(args.logs)
    if args.glob:
        paths.extend(sorted(glob.glob(args.glob)))
    if not paths:
        ap.error("give at least one log path or --glob")
    return analyze(paths, args.csv)


if __name__ == "__main__":
    sys.exit(main())

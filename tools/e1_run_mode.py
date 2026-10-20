#!/usr/bin/env python3
"""e1_run_mode.py - drive one complete E1 capture-mode run end to end.

One invocation = one capture mode. It:

  1. (optional) flashes the mode's build dir to both boards;
  2. records a dual-port monitor log at 921600 baud into data/raw/<run-id>/;
  3. splits it per port (one clock domain per pcap);
  4. converts each port log to a radiotap pcap (real RSSI/channel);
  5. runs analyze_pcap.py (length histogram, entropy, retries) and
     mode_report.py (tshark field export, printable share, cleartext
     inventory) into data/derived/<run-id>/;
  6. appends every command, exit code and artifact path to the run manifest.

Ground rules encoded here
-------------------------
* Raw logs are written once and never edited; the manifest is sealed
  separately by tools/bench_manifest.py.
* Capture sessions are kept separate from timing sessions: this driver is
  capture-only and runs at 921600 baud because heavy ring-dump logging
  perturbs latency.
* Hopping is off in capture builds (the firmware suppresses it), so a single
  channel is recorded and reported.

IDF environment: `idf.py` must be on PATH (source tools/setup_esp_idf.ps1
first). Without it, pass --no-flash to reuse the firmware already on the
boards.

Usage
-----
  python tools/e1_run_mode.py --run-id E1_A_plain_20261017 \\
      --build-dir build_cap_plain --ports COM3,COM5 \\
      --baud 921600 --duration 300 --flash

  python tools/e1_run_mode.py --self-test
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CAPTURE_DIR = os.path.join(HERE, "capture")
RAW_ROOT = os.path.join(REPO, "data", "raw")
DERIVED_ROOT = os.path.join(REPO, "data", "derived")

PY = sys.executable


def run(cmd: list[str], log_path: str | None = None,
        echo: bool = True) -> tuple[int, str]:
    """Run a command, optionally teeing output to log_path. Returns (rc, out)."""
    if echo:
        print("  $ " + " ".join(cmd))
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    out = (proc.stdout or "") + (proc.stderr or "")
    if log_path:
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        with open(log_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(out)
    return proc.returncode, out


def tail(text: str, n: int = 12) -> str:
    lines = text.strip().splitlines()
    return "\n".join(lines[-n:])


def flash(build_dir: str, port: str, run_dir: str, records: list) -> bool:
    idf_path = os.environ.get("IDF_PATH", r"C:\esp\v6.0.2\esp-idf")
    idf_py = os.path.join(idf_path, "tools", "idf.py")
    cmd = [PY, idf_py, "-B", build_dir, f"-DSDKCONFIG={build_dir}/sdkconfig",
           "-p", port, "flash"]
    logp = os.path.join(run_dir, f"flash_{port}.log")
    rc, out = run(cmd, logp)
    records.append({"step": f"flash {port}", "cmd": cmd, "rc": rc,
                    "log": os.path.relpath(logp, REPO)})
    if rc != 0:
        print(f"  [FAIL] flash {port} rc={rc}\n{tail(out)}")
        return False
    m = re.search(r"Hash of data verified", out)
    print(f"  [ OK ] flashed {port}" + (" (hash verified)" if m else ""))
    return True


def monitor(ports: list[str], baud: int, duration: int, run_dir: str,
            records: list) -> str | None:
    log = os.path.join(run_dir, "capture_dual.log")
    cmd = [PY, os.path.join(HERE, "ceepew_monitor.py")]
    for p in ports:
        cmd += ["--port", p]
    cmd += ["--baud", str(baud), "--duration", str(duration),
            "--log", os.path.relpath(log, REPO), "--no-color"]
    print(f"  monitoring {len(ports)} port(s) for {duration}s @ {baud} baud …")
    rc, out = run(cmd)
    records.append({"step": "monitor", "cmd": cmd, "rc": rc,
                    "log": os.path.relpath(log, REPO)})
    if rc != 0:
        print(f"  [WARN] monitor rc={rc}")
    if not os.path.exists(log):
        print("  [FAIL] no monitor log produced")
        return None
    size = os.path.getsize(log)
    print(f"  [ OK ] {os.path.relpath(log, REPO)} ({size} B)")
    return log


def pipeline(run_dir: str, derived: str, ports: list[str], log: str,
             records: list, verbose: bool) -> dict:
    """split -> pcap -> analyze -> mode_report, per port."""
    os.makedirs(derived, exist_ok=True)
    artifacts = {"logs": {}, "pcaps": {}, "hist_csv": {}, "mode_report": {}}

    cmd = [PY, os.path.join(CAPTURE_DIR, "split_dual_log.py"), log,
           "--outdir", os.path.relpath(run_dir, REPO), "--prefix", "capture"]
    rc, out = run(cmd)
    records.append({"step": "split_dual_log", "cmd": cmd, "rc": rc})
    print(tail(out, 8))

    for p in ports:
        plog = os.path.join(run_dir, f"capture_{p}.log")
        if not os.path.exists(plog):
            print(f"  [WARN] no split log for {p}")
            continue
        pcap = os.path.join(run_dir, f"capture_{p}.pcap")
        cmd = [PY, os.path.join(CAPTURE_DIR, "serial_to_pcap.py"), plog,
               "-o", pcap, "--radiotap"]
        rc, out = run(cmd)
        records.append({"step": f"serial_to_pcap {p}", "cmd": cmd, "rc": rc})
        m = re.search(r"parsed=(\d+).*?len_mismatch=(\d+).*?non_full_size=(\d+)",
                      out, re.S)
        conv = {"parsed": None, "len_mismatch": None, "non_full_size": None}
        if m:
            conv = {"parsed": int(m.group(1)), "len_mismatch": int(m.group(2)),
                    "non_full_size": int(m.group(3))}
        print(f"  {p}: pcap frames={conv['parsed']} "
              f"len_mismatch={conv['len_mismatch']} "
              f"non_full={conv['non_full_size']}")
        artifacts["logs"][p] = os.path.relpath(plog, REPO)
        artifacts["pcaps"][p] = os.path.relpath(pcap, REPO)
        artifacts.setdefault("convert", {})[p] = conv

        if not os.path.exists(pcap) or not conv["parsed"]:
            print(f"  [WARN] empty pcap for {p} — skipping analysis")
            continue

        hist = os.path.join(derived, f"hist_{p}.csv")
        cmd = [PY, os.path.join(CAPTURE_DIR, "analyze_pcap.py"), pcap,
               "--hist-csv", os.path.relpath(hist, REPO)]
        rc, out = run(cmd)
        records.append({"step": f"analyze_pcap {p}", "cmd": cmd, "rc": rc})
        if verbose:
            print(tail(out, 20))
        artifacts["hist_csv"][p] = os.path.relpath(hist, REPO)

        cmd = [PY, os.path.join(CAPTURE_DIR, "mode_report.py"), pcap,
               "--mode", f"{os.path.basename(run_dir)} / {p}",
               "--derived-dir", os.path.relpath(os.path.join(derived, p), REPO)]
        rc, out = run(cmd)
        records.append({"step": f"mode_report {p}", "cmd": cmd, "rc": rc})
        print(out.strip())
        artifacts["mode_report"][p] = os.path.relpath(
            os.path.join(derived, p, "mode_report.json"), REPO)
    return artifacts


def update_manifest(run_id: str, records: list, artifacts: dict,
                    duration: int, baud: int) -> None:
    mpath = os.path.join(RAW_ROOT, run_id, "manifest.json")
    if not os.path.exists(mpath):
        print(f"[WARN] no manifest at {mpath}; skipping manifest update")
        return
    man = json.load(open(mpath, encoding="utf-8"))
    man["commands"] = man.get("commands", []) + records
    man["artifacts"] = artifacts
    man["monitor_baud"] = baud
    man["monitor_duration_s"] = duration
    man["run_completed_utc"] = datetime.now(timezone.utc).isoformat(
        timespec="seconds")
    with open(mpath, "w", encoding="utf-8") as fh:
        json.dump(man, fh, indent=2)
        fh.write("\n")
    print(f"manifest updated: {man.get('run_id')} "
          f"({len(man['commands'])} command record(s))")


def _self_test() -> int:
    fails = []

    def check(name, cond, detail=""):
        print(f"  [{'PASS' if cond else 'FAIL'}] {name} {detail}")
        if not cond:
            fails.append(name)

    check("tail() clamps", tail("\n".join(str(i) for i in range(30)), 5)
          == "\n".join(str(i) for i in range(25, 30)))
    check("repo root resolves", os.path.isdir(os.path.join(REPO, "tools")))
    check("required scripts present",
          all(os.path.exists(os.path.join(CAPTURE_DIR, f)) for f in
              ("serial_to_pcap.py", "analyze_pcap.py", "mode_report.py",
               "split_dual_log.py")))
    check("monitor present", os.path.exists(os.path.join(HERE, "ceepew_monitor.py")))
    print(f"\n{'FAILED: ' + ', '.join(fails) if fails else 'self-test OK'}")
    return 1 if fails else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run-id", help="existing run id under data/raw/")
    ap.add_argument("--build-dir", help="e.g. build_cap_plain")
    ap.add_argument("--ports", help="comma-separated, e.g. COM3,COM5")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--duration", type=int, default=300)
    ap.add_argument("--flash", action="store_true",
                    help="flash the build dir first (needs idf.py on PATH)")
    ap.add_argument("--no-flash", action="store_true",
                    help="explicitly reuse whatever firmware is on the boards")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()
    for req in ("run_id", "build_dir", "ports"):
        if not getattr(args, req):
            ap.error(f"--{req.replace('_', '-')} is required")

    ports = [p.strip() for p in args.ports.split(",") if p.strip()]
    run_dir = os.path.join(RAW_ROOT, args.run_id)
    derived = os.path.join(DERIVED_ROOT, args.run_id)
    if not os.path.isdir(run_dir):
        print(f"[FAIL] {run_dir} does not exist — create it with "
              f"tools/bench_manifest.py new first", file=sys.stderr)
        return 2

    print("=" * 78)
    print(f"E1 RUN: {args.run_id}")
    print(f"  build dir : {args.build_dir}")
    print(f"  ports     : {ports}")
    print(f"  baud/dur  : {args.baud} / {args.duration}s")
    print("=" * 78)

    records: list = []
    if args.flash:
        # Verify idf.py exists via IDF_PATH rather than PATH lookup
        idf_path_check = os.environ.get("IDF_PATH", r"C:\esp\v6.0.2\esp-idf")
        idf_py_check = os.path.join(idf_path_check, "tools", "idf.py")
        if not os.path.isfile(idf_py_check):
            print("[FAIL] idf.py not found — ensure ESP-IDF is installed and IDF_PATH is set, or pass --no-flash", file=sys.stderr)
            return 2
        for p in ports:
            if not flash(args.build_dir, p, run_dir, records):
                update_manifest(args.run_id, records, {}, args.duration, args.baud)
                return 1

    log = monitor(ports, args.baud, args.duration, run_dir, records)
    if log is None:
        update_manifest(args.run_id, records, {}, args.duration, args.baud)
        return 1

    artifacts = pipeline(run_dir, derived, ports, log, records, args.verbose)
    update_manifest(args.run_id, records, artifacts, args.duration, args.baud)
    print("\nDONE. Seal the run with:")
    print(f"  python tools/bench_manifest.py seal --run-id {args.run_id}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

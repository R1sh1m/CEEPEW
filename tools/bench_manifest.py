#!/usr/bin/env python3
"""tools/bench_manifest.py — run manifest + raw-evidence sealing for CEE-PEW bench work.

Implements the data-package rules:

  data/raw/<run id>/      read-only after a run   (never edited once sealed)
  data/derived/<run id>/  analysis output
  data/manifests/         roll-up index
  results/registry.json   one entry per quoted number

A run is only usable if its manifest records **all** of:

  run id, date, experiment id, roles, commit, sdkconfig hash, build type,
  channel, distance, N planned / N completed, operator, notes,
  and the SHA-256 of every raw file.

Commands
--------
  new    create data/raw/<run-id>/ and write an OPEN manifest
  seal   hash every raw file, write SHA256SUMS.txt, flip manifest to SEALED
  verify re-check a sealed run's hashes (detects post-run edits)
  list   show all runs and their state
  show   print one manifest

Examples
--------
  python tools/bench_manifest.py new --run-id E1_A_plain_20261017 \\
      --experiment E1 --capture-mode plain --build-dir build_cap_plain \\
      --n-planned 30 --channel 6 --distance-cm 20 \\
      --roles "COM3=BENCH58 sender, COM5=promiscuous capture" \\
      --notes "hopping off, fixed channel 6, 1 s inter-message"

  python tools/bench_manifest.py seal --run-id E1_A_plain_20261017
  python tools/bench_manifest.py verify --run-id E1_A_plain_20261017
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
from datetime import datetime, timezone

RAW_ROOT = os.path.join("data", "raw")
DERIVED_ROOT = os.path.join("data", "derived")
MANIFEST_INDEX = os.path.join("data", "manifests", "index.json")

SCHEMA_VERSION = 1

# Fields a manifest MUST carry before a run can be quoted in the paper.
REQUIRED_FIELDS = (
    "run_id", "experiment", "date_utc", "roles", "git_commit", "git_dirty",
    "configured_sdkconfig_sha256", "generated_sdkconfig_sha256",
    "build_dir", "build_type", "capture_mode", "channel", "distance_cm",
    "n_planned", "operator", "notes",
)


# --------------------------------------------------------------------------
# provenance helpers
# --------------------------------------------------------------------------

def _run(cmd: list[str], cwd: str | None = None) -> str:
    try:
        out = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                             timeout=30, shell=False)
        return out.stdout.strip() if out.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def git_commit() -> str:
    return _run(["git", "rev-parse", "HEAD"]) or "unknown"


def git_dirty() -> bool:
    return bool(_run(["git", "status", "--porcelain"]))


def git_describe() -> str:
    return _run(["git", "describe", "--always", "--dirty"]) or "unknown"


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def idf_version() -> str:
    for cand in (r"C:\esp\v6.0.2\esp-idf\version.txt",
                 os.path.join(os.environ.get("IDF_PATH", ""), "version.txt")):
        if cand and os.path.exists(cand):
            return open(cand, encoding="utf-8", errors="replace").read().strip()
    return _run(["idf.py", "--version"]) or "unknown"


def configured_sdkconfig_hash(build_dir: str) -> str:
    """Hash of the sdkconfig the user intended (build_dir/sdkconfig)."""
    for cand in (os.path.join(build_dir, "sdkconfig"), "sdkconfig"):
        if os.path.exists(cand):
            return sha256_file(cand)
    return "(missing)"


def generated_sdkconfig_hash(build_dir: str) -> str:
    """Hash of the sdkconfig.h the toolchain ACTUALLY compiled against.

    This is the authoritative one: it is what the compiler saw.
    """
    cand = os.path.join(build_dir, "config", "sdkconfig.h")
    return sha256_file(cand) if os.path.exists(cand) else "(missing)"


def app_binary_hash(build_dir: str) -> str:
    cand = os.path.join(build_dir, "ceepew.bin")
    return sha256_file(cand) if os.path.exists(cand) else "(missing)"


def read_generated_flags(build_dir: str) -> dict:
    """Extract the CEE-PEW/compiler flags actually compiled into the image."""
    cand = os.path.join(build_dir, "config", "sdkconfig.h")
    if not os.path.exists(cand):
        return {}
    keep = re.compile(
        r"CONFIG_(CEEPEW_[A-Z0-9_]+|COMPILER_OPTIMIZATION[A-Z0-9_]*"
        r"|ESP_DEFAULT_CPU_FREQ_MHZ|LOG_DEFAULT_LEVEL|FREERTOS_[A-Z0-9_]*HZ)\b")
    flags = {}
    for ln in open(cand, encoding="utf-8", errors="replace"):
        m = re.match(r"#define\s+(CONFIG_\S+)\s+(.+)$", ln.strip())
        if m and keep.match(m.group(1)):
            flags[m.group(1)] = m.group(2).strip()
    return dict(sorted(flags.items()))


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------

def cmd_new(args) -> int:
    run_dir = os.path.join(RAW_ROOT, args.run_id)
    if os.path.exists(os.path.join(run_dir, "manifest.json")):
        print(f"[FAIL] {run_dir}/manifest.json already exists — pick another "
              f"run-id or use `seal`/`verify`.", file=sys.stderr)
        return 2
    os.makedirs(run_dir, exist_ok=True)
    os.makedirs(os.path.join(DERIVED_ROOT, args.run_id), exist_ok=True)

    man = {
        "schema_version": SCHEMA_VERSION,
        "state": "open",
        "run_id": args.run_id,
        "experiment": args.experiment,
        "date_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "roles": args.roles,
        "git_commit": git_commit(),
        "git_describe": git_describe(),
        "git_dirty": git_dirty(),
        "dirty_paths": _run(["git", "status", "--porcelain"]).splitlines(),
        "idf_version": idf_version(),
        "configured_sdkconfig_sha256": configured_sdkconfig_hash(args.build_dir),
        "generated_sdkconfig_sha256": generated_sdkconfig_hash(args.build_dir),
        "app_binary_sha256": app_binary_hash(args.build_dir),
        "compiled_flags": read_generated_flags(args.build_dir),
        "build_dir": args.build_dir,
        "build_type": args.build_type,
        "capture_mode": args.capture_mode,
        "channel": args.channel,
        "distance_cm": args.distance_cm,
        "environment": args.environment,
        "hopping": args.hopping,
        "n_planned": args.n_planned,
        "n_completed": None,
        "n_excluded": None,
        "exclusion_rule": args.exclusion_rule,
        "operator": args.operator,
        "notes": args.notes,
        "raw_files": {},
        "expected_raw_files": args.expect,
    }
    path = os.path.join(run_dir, "manifest.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(man, fh, indent=2)
        fh.write("\n")
    print(f"created {path}  (state=open)")
    print(f"raw dir     : {run_dir}")
    print(f"derived dir : {os.path.join(DERIVED_ROOT, args.run_id)}")
    print(f"git         : {man['git_describe']} (dirty={man['git_dirty']})")
    print(f"sdkconfig.h : {man['generated_sdkconfig_sha256'][:16]}...  "
          f"mode={args.capture_mode}")
    return 0


def _hash_tree(run_dir: str) -> dict:
    out = {}
    for root, _dirs, files in os.walk(run_dir):
        for fn in files:
            if fn in ("manifest.json", "SHA256SUMS.txt"):
                continue
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, run_dir).replace("\\", "/")
            out[rel] = sha256_file(full)
    return dict(sorted(out.items()))


def cmd_seal(args) -> int:
    run_dir = os.path.join(RAW_ROOT, args.run_id)
    mpath = os.path.join(run_dir, "manifest.json")
    if not os.path.exists(mpath):
        print(f"[FAIL] no manifest at {mpath}", file=sys.stderr)
        return 2
    man = json.load(open(mpath, encoding="utf-8"))

    files = _hash_tree(run_dir)
    if not files:
        print("[WARN] sealing a run with ZERO raw files — the manifest alone "
              "is not evidence.", file=sys.stderr)

    with open(os.path.join(run_dir, "SHA256SUMS.txt"), "w",
              encoding="utf-8", newline="\n") as fh:
        for rel, digest in files.items():
            fh.write(f"{digest}  {rel}\n")

    man["raw_files"] = files
    man["raw_file_count"] = len(files)
    man["state"] = "sealed"
    man["sealed_utc"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    if args.n_completed is not None:
        man["n_completed"] = args.n_completed
    if args.n_excluded is not None:
        man["n_excluded"] = args.n_excluded
    if args.notes_append:
        man["notes"] = (man.get("notes", "") + "\n" + args.notes_append).strip()

    missing = [k for k in REQUIRED_FIELDS
               if man.get(k) in (None, "", "(missing)", [])]
    man["missing_required_fields"] = missing

    with open(mpath, "w", encoding="utf-8") as fh:
        json.dump(man, fh, indent=2)
        fh.write("\n")

    print(f"sealed {run_dir}: {len(files)} raw file(s)")
    for rel, digest in files.items():
        print(f"  {digest[:16]}...  {rel}")
    if missing:
        print(f"[WARN] manifest missing required field(s): {missing}")
        print("       -> run is NOT quotable in the paper until filled in.")
    else:
        print("manifest carries every required field -> quotable")
    _update_index()
    return 0


def cmd_verify(args) -> int:
    run_dir = os.path.join(RAW_ROOT, args.run_id)
    mpath = os.path.join(run_dir, "manifest.json")
    if not os.path.exists(mpath):
        print(f"[FAIL] no manifest at {mpath}", file=sys.stderr)
        return 2
    man = json.load(open(mpath, encoding="utf-8"))
    recorded = man.get("raw_files", {})
    actual = _hash_tree(run_dir)

    bad = 0
    for rel in sorted(set(recorded) | set(actual)):
        r, a = recorded.get(rel), actual.get(rel)
        if r is None:
            print(f"  [EXTRA]   {rel}  (added after sealing)")
            bad += 1
        elif a is None:
            print(f"  [MISSING] {rel}")
            bad += 1
        elif r != a:
            print(f"  [CHANGED] {rel}\n              was {r}\n              now {a}")
            bad += 1
    if bad:
        print(f"[FAIL] {bad} integrity problem(s) in {run_dir}")
        return 1
    print(f"[ OK ] {run_dir}: all {len(recorded)} raw file(s) match their "
          f"recorded SHA-256 (state={man.get('state')})")
    return 0


def _update_index() -> None:
    os.makedirs(os.path.dirname(MANIFEST_INDEX), exist_ok=True)
    runs = []
    if os.path.isdir(RAW_ROOT):
        for name in sorted(os.listdir(RAW_ROOT)):
            mpath = os.path.join(RAW_ROOT, name, "manifest.json")
            if os.path.exists(mpath):
                m = json.load(open(mpath, encoding="utf-8"))
                runs.append({k: m.get(k) for k in (
                    "run_id", "state", "experiment", "date_utc", "capture_mode",
                    "n_planned", "n_completed", "raw_file_count",
                    "missing_required_fields", "notes")})
    with open(MANIFEST_INDEX, "w", encoding="utf-8") as fh:
        json.dump({"updated_utc": datetime.now(timezone.utc)
                   .isoformat(timespec="seconds"), "runs": runs}, fh, indent=2)
        fh.write("\n")
    print(f"updated {MANIFEST_INDEX} ({len(runs)} run(s))")


def cmd_list(args) -> int:
    if not os.path.isdir(RAW_ROOT):
        print("(no data/raw yet)")
        return 0
    rows = []
    for name in sorted(os.listdir(RAW_ROOT)):
        mpath = os.path.join(RAW_ROOT, name, "manifest.json")
        if not os.path.exists(mpath):
            rows.append((name, "NO MANIFEST", "", "", "", ""))
            continue
        m = json.load(open(mpath, encoding="utf-8"))
        rows.append((name, m.get("state", "?"), m.get("experiment", ""),
                     m.get("capture_mode", ""), str(m.get("n_planned", "")),
                     str(m.get("raw_file_count", 0))))
    print(f"{'run id':<34} {'state':<9} {'exp':<5} {'mode':<8} "
          f"{'Nplan':>6} {'files':>6}")
    for r in rows:
        print(f"{r[0]:<34} {r[1]:<9} {r[2]:<5} {r[3]:<8} {r[4]:>6} {r[5]:>6}")
    return 0


def cmd_show(args) -> int:
    mpath = os.path.join(RAW_ROOT, args.run_id, "manifest.json")
    if not os.path.exists(mpath):
        print(f"[FAIL] no manifest at {mpath}", file=sys.stderr)
        return 2
    print(json.dumps(json.load(open(mpath, encoding="utf-8")), indent=2))
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("new", help="create a run raw dir + OPEN manifest")
    p.add_argument("--run-id", required=True)
    p.add_argument("--experiment", required=True, help="E1..E12")
    p.add_argument("--build-dir", default="build", help="build dir compiled")
    p.add_argument("--build-type", default="capture-bench",
                   choices=["capture-bench", "diagnostic", "production", "host"])
    p.add_argument("--capture-mode", default="n/a",
                   choices=["plain", "ccmp", "ceepew", "n/a"])
    p.add_argument("--channel", default="?")
    p.add_argument("--distance-cm", default="?")
    p.add_argument("--environment", default="normal-office-wifi")
    p.add_argument("--hopping", default="off", choices=["off", "on"])
    p.add_argument("--n-planned", default="?")
    p.add_argument("--roles", default="?", help="which board did what")
    p.add_argument("--operator", default=os.environ.get("USERNAME", "agent"))
    p.add_argument("--notes", default="")
    p.add_argument("--exclusion-rule", default="",
                   help="written BEFORE looking at results")
    p.add_argument("--expect", nargs="*", default=[],
                   help="raw files this run is supposed to produce")
    p.set_defaults(func=cmd_new)

    p = sub.add_parser("seal", help="hash raw files, flip manifest to SEALED")
    p.add_argument("--run-id", required=True)
    p.add_argument("--n-completed", type=int, default=None)
    p.add_argument("--n-excluded", type=int, default=None)
    p.add_argument("--notes-append", default="")
    p.set_defaults(func=cmd_seal)

    p = sub.add_parser("verify", help="re-check a sealed run's hashes")
    p.add_argument("--run-id", required=True)
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("list", help="list all runs")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("show", help="print one manifest")
    p.add_argument("--run-id", required=True)
    p.set_defaults(func=cmd_show)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

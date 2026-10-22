#!/usr/bin/env python3
"""tools/campaign/manifest.py - sealed campaign manifest emitter.

A measurement that is not bound to a sealed manifest does not enter the
results registry. The manifest pins every input that could otherwise drift
between the run and the write-up:

  * the git revision and whether the tree was dirty at run time,
  * the SHA-256 of sdkconfig and sdkconfig.defaults,
  * the ESP-IDF release and the Python/toolchain versions,
  * the firmware image and its SHA-256,
  * the identity (role, port, MAC) of every board in the run,
  * the runner script and its SHA-256,
  * the SHA-256 and byte length of every artifact the run produced,
  * the UTC start and end of the measurement window.

The manifest is written with sorted keys and a trailing newline so that two
identical runs produce byte-identical manifests except for the timestamps.

Usage:
    python tools/campaign/manifest.py --campaign X1_timing_20261022 \
        --board "Device A:COM3:8c:94:df:97:d0:c0" \
        --board "Device B:COM4:1c:c3:ab:c2:52:ac" \
        --runner tools/campaign/run_x1_timing.py \
        --artifact data/raw/X1_timing_20261022/capture_DEVICE_A.log \
        --artifact data/raw/X1_timing_20261022/capture_DEVICE_B.log \
        --firmware build/ceepew.bin

    python tools/campaign/manifest.py --verify data/manifests/X1_timing_20261022.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
from datetime import datetime, timezone
from typing import Any, Dict, Iterable, List, Optional, Tuple

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEFAULT_MANIFEST_DIR = os.path.join(PROJECT_ROOT, "data", "manifests")

MANIFEST_SCHEMA = "ceepew-campaign-manifest/1"


# --------------------------------------------------------------------------
# primitives
# --------------------------------------------------------------------------
def sha256_file(path: str) -> str:
    """Return the hex SHA-256 of a file, or "" when the file is missing."""
    if not path or not os.path.isfile(path):
        return ""
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def utc_now_iso() -> str:
    """Current UTC time as an ISO-8601 string with an explicit Z suffix."""
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _run(cmd: List[str], cwd: Optional[str] = None) -> str:
    try:
        res = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return ""
    if res.returncode != 0:
        return ""
    return res.stdout.strip()


def git_state(repo: str = PROJECT_ROOT) -> Dict[str, Any]:
    """Return the git revision, branch and dirty flag of the working tree."""
    sha = _run(["git", "rev-parse", "HEAD"], repo)
    branch = _run(["git", "rev-parse", "--abbrev-ref", "HEAD"], repo)
    status = _run(["git", "status", "--porcelain"], repo)
    return {
        "sha": sha,
        "branch": branch,
        "dirty": bool(status),
        "dirty_paths": status.splitlines() if status else [],
    }


def idf_version() -> str:
    """Return the ESP-IDF release string, or 'unknown' when unavailable."""
    idf_path = os.environ.get("IDF_PATH", "")
    if idf_path:
        version_file = os.path.join(idf_path, "version.txt")
        if os.path.isfile(version_file):
            try:
                with open(version_file, "r", encoding="utf-8", errors="replace") as fh:
                    text = fh.read().strip()
                if text:
                    return text
            except OSError:
                pass
        git = git_state(idf_path)
        if git.get("sha"):
            return "git:" + git["sha"][:12]
    return "unknown"


def parse_board(spec: str) -> Dict[str, str]:
    """Parse a board specification string.

    Accepted forms:
        "Device A:COM3:8c:94:df:97:d0:c0"
        "Device A:COM3"
        "Device A"
    """
    parts = [p.strip() for p in spec.split(":")]
    board: Dict[str, str] = {"role": parts[0] if parts else "unknown"}
    if len(parts) > 1 and parts[1]:
        board["port"] = parts[1]
    if len(parts) > 2 and parts[2]:
        board["wifi_mac"] = parts[2].lower()
    if len(parts) > 3 and parts[3]:
        board["ble_mac"] = parts[3].lower()
    return board


def artifact_record(path: str, root: str = PROJECT_ROOT) -> Dict[str, Any]:
    """Describe one artifact: relative path, byte length and SHA-256."""
    abs_path = path if os.path.isabs(path) else os.path.join(root, path)
    try:
        rel = os.path.relpath(abs_path, root).replace("\\", "/")
    except ValueError:
        rel = abs_path.replace("\\", "/")
    rec: Dict[str, Any] = {"path": rel, "exists": os.path.isfile(abs_path)}
    if rec["exists"]:
        rec["bytes"] = os.path.getsize(abs_path)
        rec["sha256"] = sha256_file(abs_path)
    else:
        rec["bytes"] = None
        rec["sha256"] = ""
    return rec


# --------------------------------------------------------------------------
# manifest construction
# --------------------------------------------------------------------------
def build_manifest(
    campaign_id: str,
    artifacts: Iterable[str],
    boards: Iterable[str],
    runner: str = "",
    firmware: str = "",
    build_dir: str = "",
    notes: str = "",
    extra: Optional[Dict[str, Any]] = None,
    utc_start: Optional[str] = None,
    utc_end: Optional[str] = None,
) -> Dict[str, Any]:
    """Assemble a manifest dictionary. Nothing here is guessed: every field
    is either read from the filesystem or copied from the caller."""
    git = git_state()
    sdkconfig = os.path.join(PROJECT_ROOT, "sdkconfig")
    defaults = os.path.join(PROJECT_ROOT, "sdkconfig.defaults")

    manifest: Dict[str, Any] = {
        "schema": MANIFEST_SCHEMA,
        "campaign_id": campaign_id,
        "utc_start": utc_start or utc_now_iso(),
        "utc_end": utc_end or utc_now_iso(),
        "git": git,
        "sdkconfig": {
            "path": "sdkconfig",
            "sha256": sha256_file(sdkconfig),
            "defaults_sha256": sha256_file(defaults),
        },
        "toolchain": {
            "idf_version": idf_version(),
            "idf_path": os.environ.get("IDF_PATH", ""),
            "python": platform.python_version(),
            "platform": platform.platform(),
        },
        "build": {
            "build_dir": build_dir.replace("\\", "/") if build_dir else "",
            "firmware": artifact_record(firmware) if firmware else {},
        },
        "boards": [parse_board(b) for b in boards],
        "runner": (
            {
                "path": runner.replace("\\", "/"),
                "sha256": sha256_file(os.path.join(PROJECT_ROOT, runner)),
            }
            if runner
            else {}
        ),
        "artifacts": [artifact_record(a) for a in artifacts],
        "notes": notes,
    }
    if extra:
        manifest["extra"] = extra
    return manifest


def write_manifest(manifest: Dict[str, Any], out_dir: str = DEFAULT_MANIFEST_DIR) -> str:
    """Write a manifest atomically and return its path."""
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, manifest["campaign_id"] + ".json")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True, ensure_ascii=True)
        fh.write("\n")
    os.replace(tmp, path)
    return path


def load_manifest(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def verify_manifest(path: str) -> Tuple[bool, List[str]]:
    """Re-hash every artifact of a manifest.

    Returns (ok, problems). A manifest whose artifacts no longer match is
    not evidence for anything, so this is the check that runs before any
    registry entry is accepted.
    """
    problems: List[str] = []
    try:
        manifest = load_manifest(path)
    except (OSError, ValueError) as exc:
        return False, ["manifest unreadable: %s" % exc]

    for art in manifest.get("artifacts", []):
        if not art.get("exists"):
            problems.append("artifact missing: %s" % art.get("path"))
            continue
        actual = sha256_file(os.path.join(PROJECT_ROOT, art["path"]))
        if actual != art.get("sha256"):
            problems.append(
                "artifact hash mismatch: %s (manifest %s, actual %s)"
                % (art.get("path"), art.get("sha256"), actual)
            )

    runner = manifest.get("runner") or {}
    if runner.get("path"):
        actual = sha256_file(os.path.join(PROJECT_ROOT, runner["path"]))
        if actual != runner.get("sha256"):
            problems.append(
                "runner hash mismatch: %s (manifest %s, actual %s)"
                % (runner["path"], runner.get("sha256"), actual)
            )

    build = manifest.get("build") or {}
    firmware = build.get("firmware") or {}
    if firmware.get("path"):
        actual = sha256_file(os.path.join(PROJECT_ROOT, firmware["path"]))
        if actual != firmware.get("sha256"):
            problems.append(
                "firmware hash mismatch: %s (manifest %s, actual %s)"
                % (firmware["path"], firmware.get("sha256"), actual)
            )

    return (not problems), problems


def manifest_id(path: str) -> str:
    """Return the campaign id stored in a manifest file."""
    try:
        return str(load_manifest(path).get("campaign_id", ""))
    except (OSError, ValueError):
        return ""


def find_manifest(campaign_id: str, manifest_dir: str = DEFAULT_MANIFEST_DIR) -> Optional[str]:
    """Return the path of the manifest for a campaign id, if one exists."""
    candidate = os.path.join(manifest_dir, campaign_id + ".json")
    return candidate if os.path.isfile(candidate) else None


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Emit or verify a CEE-PEW campaign manifest.")
    parser.add_argument("--campaign", help="campaign id (used as the file name)")
    parser.add_argument("--board", action="append", default=[], help="role[:port[:wifi_mac[:ble_mac]]]")
    parser.add_argument("--runner", default="", help="path of the runner script")
    parser.add_argument("--artifact", action="append", default=[], help="artifact produced by the run")
    parser.add_argument("--firmware", default="", help="firmware image used for the run")
    parser.add_argument("--build-dir", default="", help="build directory used for the run")
    parser.add_argument("--notes", default="", help="free-text note stored in the manifest")
    parser.add_argument("--out-dir", default=DEFAULT_MANIFEST_DIR)
    parser.add_argument("--verify", default="", help="verify an existing manifest and exit")
    args = parser.parse_args(argv)

    if args.verify:
        ok, problems = verify_manifest(args.verify)
        print("manifest: %s" % args.verify)
        for problem in problems:
            print("  PROBLEM: %s" % problem)
        print("VERDICT: %s" % ("OK" if ok else "FAILED"))
        return 0 if ok else 1

    if not args.campaign:
        parser.error("--campaign is required unless --verify is given")

    manifest = build_manifest(
        campaign_id=args.campaign,
        artifacts=args.artifact,
        boards=args.board,
        runner=args.runner,
        firmware=args.firmware,
        build_dir=args.build_dir,
        notes=args.notes,
    )
    path = write_manifest(manifest, args.out_dir)
    print("wrote %s" % os.path.relpath(path, PROJECT_ROOT).replace("\\", "/"))
    ok, problems = verify_manifest(path)
    for problem in problems:
        print("  PROBLEM: %s" % problem)
    print("VERDICT: %s" % ("OK" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

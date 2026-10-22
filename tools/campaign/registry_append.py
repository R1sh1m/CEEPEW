#!/usr/bin/env python3
"""tools/campaign/registry_append.py - manifest-bound registry writer.

results/registry.json is the single source of truth for every quantitative
claim in the manuscript. This tool is the only sanctioned way to add an
entry produced by a measurement campaign, and it enforces the rules that
make the ledger auditable:

  1. Every appended entry names a sealed campaign manifest, and that
     manifest is re-verified (every artifact SHA-256 re-hashed) before the
     write is accepted.
  2. An entry that cites an artifact must cite one that the manifest
     lists, with the same SHA-256.
  3. The evidence class must be one of M-HW, M-HOST, C, S, E. Class U
     (unsupported) is rejected outright.
  4. Ids are unique. A duplicate id is refused rather than overwritten.
  5. The write is atomic, so an interrupted run cannot leave a truncated
     registry behind.

Usage:
    python tools/campaign/registry_append.py --manifest data/manifests/X1.json \
        --id X1-ED25519-VERIFY-MEDIAN --description "Ed25519 verify median" \
        --value 662700 --unit us --class M-HW --n 98 \
        --artifact data/raw/X1/capture_DEVICE_A.log \
        --script tools/campaign/run_x1_timing.py --reference "Section VI.A"

    python tools/campaign/registry_append.py --verify-all
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any, Dict, List, Optional

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import manifest as manifest_mod  # noqa: E402

REGISTRY_PATH = os.path.join(PROJECT_ROOT, "results", "registry.json")
VALID_CLASSES = ("M-HW", "M-HOST", "C", "S", "E")


class RegistryError(RuntimeError):
    """Raised when an entry fails validation and must not be written."""


def load_registry(path: str = REGISTRY_PATH) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def write_registry(registry: Dict[str, Any], path: str = REGISTRY_PATH) -> None:
    """Write the registry atomically with the project's canonical formatting."""
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(registry, fh, indent=2, ensure_ascii=True)
        fh.write("\n")
    os.replace(tmp, path)


def _artifact_index(manifest: Dict[str, Any]) -> Dict[str, str]:
    index: Dict[str, str] = {}
    for art in manifest.get("artifacts", []):
        path = art.get("path", "")
        if path:
            index[path.replace("\\", "/")] = art.get("sha256", "")
    return index


def validate_entry(entry: Dict[str, Any], manifest: Dict[str, Any], existing_ids: List[str]) -> None:
    """Raise RegistryError when the entry may not be written."""
    entry_id = str(entry.get("id", "")).strip()
    if not entry_id:
        raise RegistryError("entry has no id")
    if entry_id in existing_ids:
        raise RegistryError("duplicate registry id: %s" % entry_id)

    cls = str(entry.get("class", "")).strip()
    if cls not in VALID_CLASSES:
        raise RegistryError(
            "evidence class must be one of %s (got %r); class U is forbidden"
            % (", ".join(VALID_CLASSES), cls)
        )

    if entry.get("value") in (None, ""):
        raise RegistryError("entry has no value")
    if not entry.get("unit"):
        raise RegistryError("entry has no unit")

    source = entry.get("source") or {}
    if not source.get("script"):
        raise RegistryError("entry has no source.script")
    if not source.get("reference"):
        raise RegistryError("entry has no source.reference")

    artifact = entry.get("artifact")
    if artifact:
        artifacts = _artifact_index(manifest)
        key = str(artifact).replace("\\", "/")
        if key not in artifacts:
            raise RegistryError(
                "artifact %s is not listed in manifest %s"
                % (key, manifest.get("campaign_id"))
            )
        if artifacts[key] and entry.get("artifact_sha256") and entry["artifact_sha256"] != artifacts[key]:
            raise RegistryError("artifact sha256 disagrees with the manifest")


def append_entry(
    entry: Dict[str, Any],
    manifest_path: str,
    registry_path: str = REGISTRY_PATH,
    dry_run: bool = False,
) -> Dict[str, Any]:
    """Validate and append one entry. Returns the entry as written."""
    if not os.path.isfile(manifest_path):
        raise RegistryError("manifest not found: %s" % manifest_path)

    ok, problems = manifest_mod.verify_manifest(manifest_path)
    if not ok:
        raise RegistryError(
            "manifest verification failed; refusing to write:\n  " + "\n  ".join(problems)
        )

    manifest = manifest_mod.load_manifest(manifest_path)
    registry = load_registry(registry_path)
    entries = registry.setdefault("entries", [])
    existing_ids = [str(e.get("id", "")) for e in entries]

    validate_entry(entry, manifest, existing_ids)

    stored = dict(entry)
    stored["manifest"] = manifest.get("campaign_id")
    stored["manifest_path"] = os.path.relpath(manifest_path, PROJECT_ROOT).replace("\\", "/")
    stored["manifest_utc_start"] = manifest.get("utc_start")
    stored["manifest_utc_end"] = manifest.get("utc_end")

    if dry_run:
        return stored

    entries.append(stored)
    registry["updated_utc"] = manifest_mod.utc_now_iso()
    write_registry(registry, registry_path)
    return stored


def verify_registry(registry_path: str = REGISTRY_PATH) -> List[str]:
    """Check that every manifest-bound entry still resolves. Returns problems."""
    problems: List[str] = []
    registry = load_registry(registry_path)
    for entry in registry.get("entries", []):
        manifest_path = entry.get("manifest_path")
        if not manifest_path:
            continue
        abs_manifest = os.path.join(PROJECT_ROOT, manifest_path)
        if not os.path.isfile(abs_manifest):
            problems.append("%s: manifest missing (%s)" % (entry.get("id"), manifest_path))
            continue
        ok, issues = manifest_mod.verify_manifest(abs_manifest)
        if not ok:
            problems.append("%s: %s" % (entry.get("id"), "; ".join(issues)))
        artifact = entry.get("artifact")
        if artifact:
            manifest = manifest_mod.load_manifest(abs_manifest)
            index = _artifact_index(manifest)
            key = str(artifact).replace("\\", "/")
            if key not in index:
                problems.append("%s: artifact %s not in manifest" % (entry.get("id"), key))
    return problems


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Append a manifest-bound entry to results/registry.json.")
    parser.add_argument("--manifest", default="", help="campaign manifest path")
    parser.add_argument("--id", default="", help="registry entry id")
    parser.add_argument("--description", default="", help="what the number measures")
    parser.add_argument("--value", default="", help="the measured value")
    parser.add_argument("--unit", default="", help="unit of the value")
    parser.add_argument("--class", dest="evidence_class", default="", help="M-HW | M-HOST | C | S | E")
    parser.add_argument("--n", type=int, default=None, help="sample size")
    parser.add_argument("--formula", default="", help="formula, when the value is computed")
    parser.add_argument("--artifact", default="", help="artifact path (must be listed in the manifest)")
    parser.add_argument("--artifact-sha256", default="", help="expected artifact sha256")
    parser.add_argument("--script", default="", help="script that produced the value")
    parser.add_argument("--reference", default="", help="manuscript section that uses the value")
    parser.add_argument("--registry", default=REGISTRY_PATH)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--verify-all", action="store_true", help="verify every manifest-bound entry")
    args = parser.parse_args(argv)

    if args.verify_all:
        problems = verify_registry(args.registry)
        for problem in problems:
            print("PROBLEM: %s" % problem)
        print("VERDICT: %s" % ("OK" if not problems else "FAILED"))
        return 0 if not problems else 1

    if not args.manifest:
        parser.error("--manifest is required")
    missing = [
        name
        for name, value in (
            ("--id", args.id),
            ("--description", args.description),
            ("--value", args.value),
            ("--unit", args.unit),
            ("--class", args.evidence_class),
            ("--script", args.script),
            ("--reference", args.reference),
        )
        if not value
    ]
    if missing:
        parser.error("missing required arguments: %s" % ", ".join(missing))

    entry: Dict[str, Any] = {
        "id": args.id,
        "description": args.description,
        "value": args.value,
        "unit": args.unit,
        "class": args.evidence_class,
        "n": args.n,
        "source": {"script": args.script, "reference": args.reference},
    }
    if args.formula:
        entry["formula"] = args.formula
    if args.artifact:
        entry["artifact"] = args.artifact.replace("\\", "/")
    if args.artifact_sha256:
        entry["artifact_sha256"] = args.artifact_sha256

    try:
        stored = append_entry(entry, args.manifest, args.registry, dry_run=args.dry_run)
    except RegistryError as exc:
        print("REFUSED: %s" % exc)
        return 1

    print(json.dumps(stored, indent=2, sort_keys=True))
    print("DRY RUN: nothing written" if args.dry_run else "WRITTEN to %s" % args.registry)
    return 0


if __name__ == "__main__":
    sys.exit(main())

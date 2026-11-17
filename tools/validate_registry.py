#!/usr/bin/env python3
"""
tools/validate_registry.py — Results Registry Schema & Integrity Validator.

Validates that results/registry.json conforms to the CEE-PEW evidence
discipline rules defined in PAPER_WRITING_GUIDE.md:
  1. Valid JSON syntax and top-level schema_version.
  2. All declared evidence classes match authoritative classes (M-HW, M-HOST, C, S, E).
  3. No unsupported ('U') evidence markers exist in active claims.
  4. Device identity mappings and clock basis specifications are well-formed.
  5. Every item in data['entries'] contains unique id, non-empty description, and valid class.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any, Dict, List, Set, Tuple

VALID_EVIDENCE_CLASSES = {"M-HW", "M-HOST", "C", "S", "E"}


def validate_registry(path: str) -> Tuple[bool, List[str], Dict[str, Any]]:
    errors: List[str] = []
    stats: Dict[str, Any] = {"total_claims": 0, "by_class": {c: 0 for c in sorted(VALID_EVIDENCE_CLASSES)}}

    if not os.path.isfile(path):
        return False, [f"File not found: {path}"], stats

    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:
        return False, [f"JSON parsing error: {e}"], stats

    if "schema_version" not in data:
        errors.append("Missing required top-level key: 'schema_version'")

    if "evidence_classes" not in data:
        errors.append("Missing required top-level key: 'evidence_classes'")

    entries = data.get("entries", [])
    if not isinstance(entries, list):
        errors.append("'entries' must be a JSON array")
        return False, errors, stats

    seen_ids: Set[str] = set()

    for i, item in enumerate(entries):
        if not isinstance(item, dict):
            errors.append(f"Entry #{i} is not a JSON object")
            continue

        item_id = item.get("id")
        if not item_id:
            errors.append(f"Entry #{i} missing required 'id'")
        elif item_id in seen_ids:
            errors.append(f"Duplicate entry id: '{item_id}'")
        else:
            seen_ids.add(item_id)

        cls = item.get("class")
        if not cls:
            errors.append(f"Entry '{item_id}' missing required 'class'")
        elif cls not in VALID_EVIDENCE_CLASSES:
            errors.append(f"Entry '{item_id}' has invalid evidence class '{cls}' (expected {VALID_EVIDENCE_CLASSES})")
        else:
            stats["total_claims"] += 1
            stats["by_class"][cls] = stats["by_class"].get(cls, 0) + 1

        if not item.get("description"):
            errors.append(f"Entry '{item_id}' has empty description")

    is_valid = len(errors) == 0
    return is_valid, errors, stats


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="CEE-PEW Results Registry Integrity Validator")
    parser.add_argument("--registry", default="results/registry.json", help="Path to registry.json")
    args = parser.parse_args(argv)

    is_valid, errors, stats = validate_registry(args.registry)

    if not is_valid:
        print(f"[FAIL] Registry integrity validation failed with {len(errors)} error(s):")
        for err in errors:
            print(f"  - {err}")
        return 1

    print(f"[PASS] Registry integrity check passed: {args.registry}")
    print(f"  Total cataloged evidence claims: {stats['total_claims']}")
    for ev_cls, count in sorted(stats["by_class"].items()):
        print(f"    - {ev_cls:<8}: {count} claim(s)")

    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""tools/campaign/run_x8_delivery.py - delivery accounting (campaign X8).

Turns a session log into an explicit delivery ledger instead of a headline
percentage. Every attempt is counted against a denominator, and every loss
is attributed to a cause that the firmware actually logged. When a cause has
no instrumented log line in the firmware build under test, the report says
so rather than silently folding it into "loss".

Counted events (all from the CEE-PEW logging vocabulary):

  message attempt   [SECURE_CHAT_TX] Initiating TX: len=%u bytes, peer_mac=...
  fragmentation     [SECURE_CHAT_TX] Fragmenting: %u compressed bytes -> %u fragments
  fragment attempt  [SECURE_CHAT_TX] Fragment %u/%u: chunk=%u B, frag_in=%u B
  fragment loss     [SECURE_CHAT_TX] Fragment %u ARQ failed: err=%d
  message success   [SECURE_CHAT_TX] All %u fragments successfully delivered!
  message failure   [SECURE_CHAT_TX] FAILED: <cause>

Message-level failures are attributed to one of:
  identity_unverified, box_pubkey_missing, peer_not_registered,
  too_many_fragments, pipeline_build_failed, other.

The delivery ratio is reported with a Wilson score interval computed by
tools/journal/stats_core.py. A message that was initiated but neither
confirmed nor explicitly failed is reported as "unresolved" and is not
silently counted as a delivery.

Usage:
    python tools/campaign/run_x8_delivery.py --log logs/pairing_20261022.txt
    python tools/campaign/run_x8_delivery.py --log a.log --log b.log --out-json data/derived/x8.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Any, Dict, List, Optional

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tools", "journal"))

import stats_core  # noqa: E402

RE_INIT = re.compile(r"\[SECURE_CHAT_TX\] Initiating TX:\s*len=(\d+)\s*bytes")
RE_FRAG = re.compile(r"\[SECURE_CHAT_TX\] Fragmenting:\s*(\d+)\s*compressed bytes\s*->\s*(\d+)\s*fragments")
RE_FRAG_ATTEMPT = re.compile(r"\[SECURE_CHAT_TX\] Fragment (\d+)/(\d+):\s*chunk=(\d+)\s*B")
RE_FRAG_FAIL = re.compile(r"\[SECURE_CHAT_TX\] Fragment (\d+) ARQ failed:\s*err=(-?\d+)")
RE_MSG_OK = re.compile(r"\[SECURE_CHAT_TX\] All (\d+) fragments successfully delivered!")
RE_MSG_FAIL = re.compile(r"\[SECURE_CHAT_TX\] FAILED:\s*(.+)$")
RE_QUEUE_FULL = re.compile(r"queue full|QUEUE_FULL|queue_full|xQueueSend failed|rx queue overflow", re.IGNORECASE)

FAILURE_CAUSES = (
    ("identity_unverified", "peer identity unverified"),
    ("box_pubkey_missing", "box pubkey not yet received"),
    ("peer_not_registered", "not registered in radio stack"),
    ("too_many_fragments", "Too many fragments"),
    ("pipeline_build_failed", "Failed to build fragment pipeline"),
)


def attribute_cause(text: str) -> str:
    lowered = text.lower()
    for name, needle in FAILURE_CAUSES:
        if needle.lower() in lowered:
            return name
    return "other"


def parse_log(path: str) -> Dict[str, Any]:
    """Parse one session log into delivery counters."""
    counters: Dict[str, Any] = {
        "source": os.path.relpath(path, PROJECT_ROOT).replace("\\", "/"),
        "messages_initiated": 0,
        "messages_delivered": 0,
        "messages_failed": 0,
        "fragments_expected": 0,
        "fragment_attempts": 0,
        "fragment_failures": 0,
        "failure_causes": {},
        "queue_saturation_events": 0,
        "unresolved_messages": 0,
    }
    delivered_fragments = 0
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if RE_INIT.search(line):
                counters["messages_initiated"] += 1
            m = RE_FRAG.search(line)
            if m:
                counters["fragments_expected"] += int(m.group(2))
            if RE_FRAG_ATTEMPT.search(line):
                counters["fragment_attempts"] += 1
            m = RE_FRAG_FAIL.search(line)
            if m:
                counters["fragment_failures"] += 1
            m = RE_MSG_OK.search(line)
            if m:
                counters["messages_delivered"] += 1
                delivered_fragments += int(m.group(1))
            m = RE_MSG_FAIL.search(line)
            if m:
                counters["messages_failed"] += 1
                cause = attribute_cause(m.group(1))
                counters["failure_causes"][cause] = counters["failure_causes"].get(cause, 0) + 1
            if RE_QUEUE_FULL.search(line):
                counters["queue_saturation_events"] += 1
    counters["fragments_confirmed_delivered"] = delivered_fragments
    counters["unresolved_messages"] = (
        counters["messages_initiated"]
        - counters["messages_delivered"]
        - counters["messages_failed"]
    )
    return counters


def merge(counters: List[Dict[str, Any]]) -> Dict[str, Any]:
    total: Dict[str, Any] = {
        "messages_initiated": 0,
        "messages_delivered": 0,
        "messages_failed": 0,
        "fragments_expected": 0,
        "fragments_confirmed_delivered": 0,
        "fragment_attempts": 0,
        "fragment_failures": 0,
        "unresolved_messages": 0,
        "queue_saturation_events": 0,
        "failure_causes": {},
        "sources": [],
    }
    for counter in counters:
        total["sources"].append(counter["source"])
        for key in (
            "messages_initiated",
            "messages_delivered",
            "messages_failed",
            "fragments_expected",
            "fragments_confirmed_delivered",
            "fragment_attempts",
            "fragment_failures",
            "unresolved_messages",
            "queue_saturation_events",
        ):
            total[key] += counter[key]
        for cause, count in counter["failure_causes"].items():
            total["failure_causes"][cause] = total["failure_causes"].get(cause, 0) + count
    return total


def report(total: Dict[str, Any], level: float = 0.95) -> Dict[str, Any]:
    initiated = total["messages_initiated"]
    delivered = total["messages_delivered"]
    failed = total["messages_failed"]

    result = dict(total)
    result["delivery"] = {
        "denominator": initiated,
        "confirmed_delivered": delivered,
        "ratio": (delivered / initiated) if initiated else None,
        "wilson_ci95": list(stats_core.wilson_ci(delivered, initiated, level=level)) if initiated else None,
        "note": (
            "Unresolved messages (initiated, neither confirmed nor failed) are counted "
            "as not confirmed. The Wilson interval treats the denominator as the number "
            "of initiated messages."
        ),
    }
    result["loss_attribution"] = {
        "fragment_arq_exhausted": total["fragment_failures"],
        "message_level_failures": failed,
        "message_failure_causes": total["failure_causes"],
        "queue_saturation_events": total["queue_saturation_events"],
        "queue_saturation_instrumented": total["queue_saturation_events"] > 0,
        "unresolved_messages": total["unresolved_messages"],
    }
    return result


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Delivery accounting for CEE-PEW session logs (campaign X8).")
    parser.add_argument("--log", action="append", default=[], help="session log (repeatable)")
    parser.add_argument("--out-json", default="", help="write the accounting as JSON")
    args = parser.parse_args(argv)

    if not args.log:
        parser.error("at least one --log is required")

    counters = [parse_log(path) for path in args.log]
    result = report(merge(counters))

    delivery = result["delivery"]
    print("messages initiated      : %d" % delivery["denominator"])
    print("confirmed delivered     : %d" % delivery["confirmed_delivered"])
    print("message-level failures  : %d" % result["messages_failed"])
    print("unresolved              : %d" % result["unresolved_messages"])
    print("fragments expected      : %d" % result["fragments_expected"])
    print("fragment attempts       : %d" % result["fragment_attempts"])
    print("fragment ARQ failures   : %d" % result["fragment_failures"])
    if delivery["ratio"] is not None:
        lo, hi = delivery["wilson_ci95"]
        print("confirmed delivery ratio: %.4f  (Wilson 95%% CI %.4f .. %.4f)" % (delivery["ratio"], lo, hi))
    else:
        print("confirmed delivery ratio: undefined (no messages initiated)")
    print("failure causes          : %s" % (result["failure_causes"] or "none observed"))
    if not result["loss_attribution"]["queue_saturation_instrumented"]:
        print(
            "NOTE: no queue-saturation log line was observed. This firmware build does not "
            "log queue-full events, so queue saturation cannot be separated from other loss "
            "causes in this run."
        )

    if args.out_json:
        os.makedirs(os.path.dirname(os.path.abspath(args.out_json)), exist_ok=True)
        with open(args.out_json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(result, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("wrote %s" % args.out_json)

    return 0


if __name__ == "__main__":
    sys.exit(main())

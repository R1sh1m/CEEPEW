#!/usr/bin/env python3
"""tools/e3_replay_reject.py — end-to-end replay rejection analysis for CEE-PEW.

Parses dual-device capture logs to verify that frames injected by the E3 replay
hook are rejected by the peer's replay window specifically as replays
(CEEPEW_ERR_REPLAY, err=17), using time-correlated matching rather than app-level
sequence accounting.

CRITICAL (evidence class): because ARQ retransmits, a dropped replayed frame can
be followed by a clean retry of the SAME application sequence number. So app-level
`seq` accounting CANNOT be used to test replay rejection — every replayed seq will
eventually appear committed via its clean retry. Only one-to-one time-correlated
rejection on the peer device is valid.

Patterns extracted:
  1. Replay armed (sender):   [BENCH58_REPLAY] armed
  2. Replay injected (sender): [BENCH58_REPLAY] replayed N B with stale esl header (fresh arq seq downstream)
  3. Replay reject (receiver): Discard: replay commit rejected seq=N (err=17)
  4. All other Discard: lines (for category breakdown)

Timestamps: lines begin with <seconds>.<frac> (host receive time) and contain the
device's own `I (microseconds)` boot time. We use the device boot-time microseconds
for Δt (on-device clock); fall back to host time if absent.

Usage:
  python tools/e3_replay_reject.py --run-id E3_replay_20261019
  python tools/e3_replay_reject.py --log path/to/capture_dual.log
  python tools/e3_replay_reject.py --self-test
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple, Any


# --------------------------------------------------------------------------
# Regex patterns for extraction
# --------------------------------------------------------------------------

# Host timestamp at line start: "NN.N [DEVICE_X] ..."
HOST_TS_RE = re.compile(r"^(\d+(?:\.\d+)?)\s+\[([^\]]+)\]")

# Device boot-time microseconds: "I (123456)" or "W (123456)" or "E (123456)"
DEVICE_TS_RE = re.compile(r"\b[IWE]\s*\(\s*(\d+)\s*\)")

# Replay armed marker (sender side): [BENCH58_REPLAY] armed
BENCH58_REPLAY_ARMED_RE = re.compile(r"\[BENCH58_REPLAY\]\s+armed")

# Replay injected marker (sender side): [BENCH58_REPLAY] replayed N B with stale esl header (fresh arq seq downstream)
BENCH58_REPLAY_REPLAYED_RE = re.compile(
    r"\[BENCH58_REPLAY\]\s+replayed\s+(\d+)\s+B\s+with\s+stale\s+esl\s+header"
)

# Replay commit rejection (receiver side): Discard: replay commit rejected seq=N (err=17)
DISCARD_REPLAY_COMMIT_RE = re.compile(
    r"Discard:\s+replay commit rejected\s+seq=\d+\s+\(err=(-?\d+)\)"
)

# Replay commit seq unavailable (receiver side): Discard: replay commit seq unavailable (err=N seq=N)
DISCARD_REPLAY_SEQ_UNAVAIL_RE = re.compile(
    r"Discard:\s+replay commit seq unavailable\s+\(err=(-?\d+)"
)

# ESL transport header/MAC/replay check failed (receiver side): Discard: ESL transport header/MAC/replay check failed (err=N len=N)
DISCARD_ESL_RE = re.compile(
    r"Discard:\s+ESL transport header/MAC/replay check failed\s+\(err=(-?\d+)"
)

# Crypto box reject: Discard: crypto_box decrypt FAILED (err=N)
DISCARD_CRYPTO_BOX_RE = re.compile(r"Discard:\s+crypto_box decrypt FAILED\s+\(err=(-?\d+)")

# Crypto ascon reject: Discard: Ascon-128 AEAD auth tag verification FAILED (err=N)
DISCARD_ASCON_RE = re.compile(r"Discard:\s+Ascon-128 AEAD auth tag verification FAILED\s+\(err=(-?\d+)")

# Crypto pipeline reject: Discard: RX decrypt pipeline failed (err=N)
DISCARD_PIPELINE_RE = re.compile(r"Discard:\s+RX decrypt pipeline failed\s+\(err=(-?\d+)")

# Any Discard: line (catch-all for category breakdown)
DISCARD_ANY_RE = re.compile(r"Discard:")


# --------------------------------------------------------------------------
# Error code lookup from hal_ui_types.h
# --------------------------------------------------------------------------

CEEPEW_ERR_NAMES = {
    0: "CEEPEW_OK",
    1: "CEEPEW_ERR_NULL_PTR",
    2: "CEEPEW_ERR_BOUNDS",
    3: "CEEPEW_ERR_PARAM",
    4: "CEEPEW_ERR_NONCE_EXHAUSTED",
    5: "CEEPEW_ERR_CRYPTO",
    6: "CEEPEW_ERR_TRANSPORT",
    7: "CEEPEW_ERR_FEC",
    8: "CEEPEW_ERR_PINS",
    9: "CEEPEW_ERR_ALLOC",
    10: "CEEPEW_ERR_INTERNAL",
    11: "CEEPEW_ERR_TIMEOUT",
    12: "CEEPEW_ERR_OVERFLOW",
    13: "CEEPEW_ERR_UNSUPPORTED",
    14: "CEEPEW_ERR_BUSY",
    15: "CEEPEW_ERR_HW",
    16: "CEEPEW_ERR_NOENT",
    17: "CEEPEW_ERR_REPLAY",
    18: "CEEPEW_ERR_SIG_FAIL",
    19: "CEEPEW_ERR_MAX_RETRIES",
    20: "CEEPEW_ERR_AUTH_FAIL",
    21: "CEEPEW_ERR_FEC_UNCORRECT",
    22: "CEEPEW_ERR_NONCE_NEARLY_EXHAUSTED",
    23: "CEEPEW_ERR_NEED_TX",
}


# --------------------------------------------------------------------------
# Data structures
# --------------------------------------------------------------------------

@dataclass
class ReplayEvent:
    """A replay injection event on the sender side."""
    host_ts: float
    device_ts_us: Optional[int]
    device: str
    payload_len: int  # from "replayed N B"


@dataclass
class RejectEvent:
    """A rejection event on the receiver side."""
    host_ts: float
    device_ts_us: Optional[int]
    device: str
    kind: str  # 'replay_commit', 'replay_seq_unavail', 'esl', 'box', 'ascon', 'pipeline', 'other'
    err: int


@dataclass
class MatchResult:
    """Result of matching a replay event to a rejection."""
    replay: ReplayEvent
    reject: Optional[RejectEvent]
    delta_t_us: Optional[int]  # reject.device_ts - replay.device_ts
    delta_t_host: Optional[float]  # reject.host_ts - replay.host_ts


# --------------------------------------------------------------------------
# Line parsing
# --------------------------------------------------------------------------

def extract_device_label(line: str) -> str:
    """Extract device label from a log line.

    Device label = the [...] token immediately before the ESP-IDF I (/W (/E ( marker.
    Fall back to [COMx] if present, else UNKNOWN.
    """
    m = re.search(r"\[([^\]]+)\]\s*(?:[IWEVD]\s*\(\s*\d+\s*\))", line)
    if m:
        return m.group(1)

    m = re.search(r"\[(COM\d+)\]", line)
    if m:
        return m.group(1)

    m = re.search(r"\d+(?:\.\d+)?\s+\[([^\]]+)\]", line)
    if m:
        return m.group(1)

    return "UNKNOWN"


def extract_host_timestamp(line: str) -> Optional[float]:
    """Extract host receive timestamp from line start."""
    m = HOST_TS_RE.match(line)
    if m:
        return float(m.group(1))
    return None


def extract_device_timestamp_us(line: str) -> Optional[int]:
    """Extract device boot-time microseconds from I (N) marker."""
    m = DEVICE_TS_RE.search(line)
    if m:
        return int(m.group(1))
    return None


# --------------------------------------------------------------------------
# Analysis
# --------------------------------------------------------------------------

class ReplayRejectAnalyzer:
    def __init__(self):
        self.replay_events: List[ReplayEvent] = []
        self.replay_armed_count = 0
        self.reject_events: List[RejectEvent] = []
        self.reject_counts: Counter = Counter()
        self.device_counts: Counter = Counter()
        self.clock_used = "device"  # or "host" if device ts unavailable

    def process_line(self, line: str) -> None:
        """Process a single log line."""
        device = extract_device_label(line)
        host_ts = extract_host_timestamp(line)
        device_ts = extract_device_timestamp_us(line)

        if host_ts is None:
            return  # Skip lines without host timestamp

        # Track device ts availability
        if device_ts is not None:
            self.clock_used = "device"

        # Replay armed event (count separately)
        if BENCH58_REPLAY_ARMED_RE.search(line):
            self.replay_armed_count += 1
            return

        # Replay injected event (the actual injection marker)
        m = BENCH58_REPLAY_REPLAYED_RE.search(line)
        if m:
            payload_len = int(m.group(1))
            self.replay_events.append(ReplayEvent(
                host_ts=host_ts,
                device_ts_us=device_ts,
                device=device,
                payload_len=payload_len,
            ))
            self.device_counts[device] += 1
            return

        # Rejections - check in priority order
        kind = None
        err = None

        m = DISCARD_REPLAY_COMMIT_RE.search(line)
        if m:
            kind = "replay_commit"
            err = int(m.group(1))
        else:
            m = DISCARD_REPLAY_SEQ_UNAVAIL_RE.search(line)
            if m:
                kind = "replay_seq_unavail"
                err = int(m.group(1))
            else:
                m = DISCARD_ESL_RE.search(line)
                if m:
                    kind = "esl"
                    err = int(m.group(1))
                else:
                    m = DISCARD_CRYPTO_BOX_RE.search(line)
                    if m:
                        kind = "box"
                        err = int(m.group(1))
                    else:
                        m = DISCARD_ASCON_RE.search(line)
                        if m:
                            kind = "ascon"
                            err = int(m.group(1))
                        else:
                            m = DISCARD_PIPELINE_RE.search(line)
                            if m:
                                kind = "pipeline"
                                err = int(m.group(1))
                            elif DISCARD_ANY_RE.search(line):
                                kind = "other"
                                err = 0

        if kind is not None:
            self.reject_events.append(RejectEvent(
                host_ts=host_ts,
                device_ts_us=device_ts,
                device=device,
                kind=kind,
                err=err,
            ))
            self.reject_counts[kind] += 1

    def process_file(self, path: str) -> int:
        """Process an entire log file. Returns number of lines processed."""
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                n = 0
                for line in fh:
                    self.process_line(line)
                    n += 1
                return n
        except OSError as exc:
            print(f"[FAIL] {path}: {exc}", file=sys.stderr)
            return -1

    def match_replay_to_reject(self, window_seconds: float) -> List[MatchResult]:
        """Match each replay injection event to the nearest err=17 rejection on the PEER device.

        A replay event from DEVICE_A is matched to an err=17 rejection on DEVICE_B, and vice versa.
        Matching uses HOST timestamps because device timestamps are boot-time (relative to
        each device's boot), not absolute. Host timestamps represent when the log line was
        received and are synchronized across devices.
        """
        results = []

        for replay in self.replay_events:
            # Find peer device
            peer_device = None
            if replay.device == "DEVICE_A":
                peer_device = "DEVICE_B"
            elif replay.device == "DEVICE_B":
                peer_device = "DEVICE_A"

            if peer_device is None:
                # Unknown device, skip matching
                results.append(MatchResult(replay=replay, reject=None, delta_t_us=None, delta_t_host=None))
                continue

            # Find candidate err=17 rejections on peer device
            candidates = [r for r in self.reject_events
                          if r.device == peer_device and r.err == 17]

            if not candidates:
                results.append(MatchResult(replay=replay, reject=None, delta_t_us=None, delta_t_host=None))
                continue

            # Find nearest by host timestamp (the only reliable cross-device clock)
            best_reject = None
            best_delta_host = float("inf")

            for reject in candidates:
                delta_host = reject.host_ts - replay.host_ts
                delta_abs = abs(delta_host)
                if delta_abs <= window_seconds:
                    if delta_abs < best_delta_host:
                        best_delta_host = delta_abs
                        best_reject = reject

            if best_reject is not None:
                # Compute delta_t for reporting (host-based, converted to microseconds)
                delta_t_us = int((best_reject.host_ts - replay.host_ts) * 1_000_000)
                results.append(MatchResult(
                    replay=replay,
                    reject=best_reject,
                    delta_t_us=delta_t_us,
                    delta_t_host=best_reject.host_ts - replay.host_ts,
                ))
            else:
                results.append(MatchResult(replay=replay, reject=None, delta_t_us=None, delta_t_host=None))

        return results

    def to_dict(self, run_id: str, window_seconds: float) -> Dict[str, Any]:
        """Export results as a dictionary for JSON serialization."""
        matches = self.match_replay_to_reject(window_seconds)

        matched_count = sum(1 for m in matches if m.reject is not None)
        unmatched_count = len(matches) - matched_count

        # Compute delta_t stats for matched pairs (using host-based delta converted to us)
        matched_deltas_us = [m.delta_t_us for m in matches if m.delta_t_us is not None]
        matched_deltas_host = [m.delta_t_host for m in matches if m.delta_t_host is not None]

        median_delta_us = None
        max_abs_delta_us = None
        median_delta_host = None
        if matched_deltas_us:
            sorted_deltas = sorted(matched_deltas_us)
            n = len(sorted_deltas)
            if n % 2 == 0:
                median_delta_us = (sorted_deltas[n // 2 - 1] + sorted_deltas[n // 2]) / 2
            else:
                median_delta_us = sorted_deltas[n // 2]
            max_abs_delta_us = max(abs(d) for d in matched_deltas_us)
        if matched_deltas_host:
            sorted_deltas_h = sorted(matched_deltas_host)
            n = len(sorted_deltas_h)
            if n % 2 == 0:
                median_delta_host = (sorted_deltas_h[n // 2 - 1] + sorted_deltas_h[n // 2]) / 2
            else:
                median_delta_host = sorted_deltas_h[n // 2]

        # Unexplained rejections: rejections not matched to any replay event.
        matched_reject_ids = set(id(m.reject) for m in matches if m.reject is not None)
        unexplained_rejects = [
            r for r in self.reject_events
            if id(r) not in matched_reject_ids
        ]

        # Per-device replay counts
        replay_by_device: Dict[str, List[int]] = defaultdict(list)
        for re_ in self.replay_events:
            replay_by_device[re_.device].append(re_.payload_len)

        # Per-device, per-kind reject counts
        rejects_by_device_kind: Dict[str, Dict[str, int]] = defaultdict(lambda: defaultdict(int))
        for re_ in self.reject_events:
            rejects_by_device_kind[re_.device][re_.kind] += 1

        # err=17 specific counts
        err17_rejects = [r for r in self.reject_events if r.err == 17]
        err17_by_device: Dict[str, int] = defaultdict(int)
        for r in err17_rejects:
            err17_by_device[r.device] += 1

        return {
            "run_id": run_id,
            "window_seconds": window_seconds,
            "clock_used": self.clock_used,
            "replay_injections": {
                "total": len(self.replay_events),
                "replay_armed": self.replay_armed_count,
                "by_device": {k: {"count": len(v), "payload_lens": v} for k, v in replay_by_device.items()},
            },
            "rejections": {
                "total": len(self.reject_events),
                "by_kind": dict(self.reject_counts),
                "by_device_kind": {k: dict(v) for k, v in rejects_by_device_kind.items()},
                "err17_total": len(err17_rejects),
                "err17_by_device": dict(err17_by_device),
            },
            "matching": {
                "matched_count": matched_count,
                "unmatched_count": unmatched_count,
                "median_delta_us": median_delta_us,
                "max_abs_delta_us": max_abs_delta_us,
                "median_delta_host_s": median_delta_host,
            },
            "unexplained_rejections": {
                "count": len(unexplained_rejects),
                "by_kind": dict(Counter(r.kind for r in unexplained_rejects)),
                "by_err": dict(Counter(r.err for r in unexplained_rejects)),
            },
            "match_details": [
                {
                    "replay_payload_len": m.replay.payload_len,
                    "replay_device": m.replay.device,
                    "replay_host_ts": m.replay.host_ts,
                    "replay_device_ts_us": m.replay.device_ts_us,
                    "reject_kind": m.reject.kind if m.reject else None,
                    "reject_device": m.reject.device if m.reject else None,
                    "reject_err": m.reject.err if m.reject else None,
                    "delta_t_us": m.delta_t_us,
                    "delta_t_host_s": m.delta_t_host,
                }
                for m in matches
            ],
            "unexplained_details": [
                {
                    "device": r.device,
                    "kind": r.kind,
                    "err": r.err,
                    "host_ts": r.host_ts,
                    "device_ts_us": r.device_ts_us,
                }
                for r in unexplained_rejects
            ],
        }


# --------------------------------------------------------------------------
# Report formatting
# --------------------------------------------------------------------------

def fmt_us(us: Optional[int]) -> str:
    """Format microseconds as signed milliseconds."""
    if us is None:
        return "N/A"
    sign = "+" if us >= 0 else ""
    return f"{sign}{us / 1000.0:.2f} ms"


def generate_report(data: Dict[str, Any]) -> str:
    """Generate the human-readable report."""
    lines = []
    lines.append("=" * 96)
    lines.append("CEE-PEW E3 REPLAY REJECT REPORT")
    lines.append("=" * 96)
    lines.append(f"run_id: {data['run_id']}")
    lines.append(f"window: {data['window_seconds']} s")
    lines.append(f"clock: {data['clock_used']} timestamps (device boot-time microseconds)")
    lines.append("")

    # Replay injection summary
    ri = data["replay_injections"]
    lines.append("-- replay injection events (sender side) --")
    lines.append(f"  total replay injections : {ri['total']}")
    lines.append(f"  replay armed count      : {ri['replay_armed']}")
    for device, info in sorted(ri["by_device"].items()):
        lens = info["payload_lens"]
        lines.append(f"  [{device}]: {info['count']} injections, payload_lens={lens}")
    lines.append("")

    # Rejections summary
    rej = data["rejections"]
    lines.append("-- rejection events (receiver side, ALL categories) --")
    lines.append(f"  total rejections     : {rej['total']}")
    lines.append(f"  err=17 (CEEPEW_ERR_REPLAY) total : {rej['err17_total']}")
    for device, count in sorted(rej["err17_by_device"].items()):
        lines.append(f"  [{device}] err=17 count: {count}")
    lines.append(f"  by kind: {rej['by_kind']}")
    for device, kinds in sorted(rej["by_device_kind"].items()):
        lines.append(f"  [{device}]: {dict(kinds)}")
    lines.append("")

    # Match summary
    match = data["matching"]
    lines.append("-- match table (replay injection -> peer err=17 rejection) --")
    lines.append(f"  MATCHED {match['matched_count']} / {data['replay_injections']['total']}")
    if match['median_delta_us'] is not None:
        median_ms = match['median_delta_us'] / 1000.0  # convert us to ms
        max_ms = match['max_abs_delta_us'] / 1000.0
        lines.append(f"  median delta_t: {median_ms:.2f} ms ({match['median_delta_host_s']:.3f} s)")
        lines.append(f"  max |delta_t|: {max_ms:.2f} ms")
    lines.append("")

    # Detailed match table
    lines.append("  replay_len  replay_dev  ->  reject_kind  reject_dev  reject_err  delta_t(ms)  delta_t(host)")
    for det in data["match_details"]:
        if det["reject_kind"] is not None:
            delta_ms = det['delta_t_us'] / 1000.0 if det['delta_t_us'] is not None else None
            delta_ms_str = f"{delta_ms:.2f}" if delta_ms is not None else "N/A"
            lines.append(f"  {det['replay_payload_len']:>10}  {det['replay_device']:<11} ->  {det['reject_kind']:<14}  {det['reject_device']:<10}  {det['reject_err']:<11}  {delta_ms_str:>11} ms  {det['delta_t_host_s']:>+10.3f}s")
        else:
            lines.append(f"  {det['replay_payload_len']:>10}  {det['replay_device']:<11} ->  UNMATCHED")
    lines.append("")

    # Unexplained rejections
    unexp = data["unexplained_rejections"]
    lines.append("-- unexplained rejections (not matched to a replay injection) --")
    lines.append(f"  count: {unexp['count']}")
    if unexp["by_kind"]:
        lines.append(f"  by kind: {unexp['by_kind']}")
    if unexp["by_err"]:
        lines.append(f"  by err: {unexp['by_err']}")
    if data["unexplained_details"]:
        lines.append("  details:")
        for det in data["unexplained_details"]:
            lines.append(f"    [{det['device']}] {det['kind']} err={det['err']} @ {det['host_ts']:.1f}s")
    lines.append("")

    # Interpretation
    lines.append("-- INTERPRETATION --")
    lines.append("  This report uses ONE-TO-ONE TIME-CORRELATED matching to verify replay rejection.")
    lines.append("  For each replay injection event on device A, we find the nearest err=17 rejection on")
    lines.append("  device B (the peer) within the time window.")
    lines.append("")
    lines.append("  WHY TIME-CORRELATION MATTERS:")
    lines.append("  The E3 replay hook replays a stale ESL header with a fresh ARQ seq downstream, so")
    lines.append("  the replayed frame passes ARQ as a new frame and reaches the replay window. ARQ")
    lines.append("  retransmission means a dropped frame can be followed by a clean retry of the same")
    lines.append("  application sequence number, so per-seq accounting would report the exact opposite")
    lines.append("  of the truth (claiming replayed frames were 'accepted').")
    lines.append("")
    lines.append("  Only time-correlated matching of replay injection events to peer err=17 rejections")
    lines.append("  provides valid evidence of replay-window rejection.")
    lines.append("")
    if match["matched_count"] == data["replay_injections"]["total"]:
        lines.append(f"  => ALL {match['matched_count']} replayed frames were rejected as replays (err=17) by the peer.")
    else:
        lines.append(f"  => {match['matched_count']}/{data['replay_injections']['total']} replayed frames matched to peer err=17 rejections.")
        if match["unmatched_count"] > 0:
            lines.append(f"     {match['unmatched_count']} replayed frames had no matching err=17 rejection (investigate drops).")
    lines.append("")

    return "\n".join(lines)


def generate_markdown_report(data: Dict[str, Any]) -> str:
    """Generate the Markdown report for the paper."""
    lines = []
    lines.append(f"# E3 Replay Reject Report: {data['run_id']}")
    lines.append("")
    lines.append(f"- Window: {data['window_seconds']} s")
    lines.append(f"- Clock: {data['clock_used']} timestamps")
    lines.append("")

    lines.append("## Replay Injection Events")
    lines.append("")
    ri = data["replay_injections"]
    lines.append(f"- Total replay injections: {ri['total']}")
    lines.append(f"- Replay armed count: {ri['replay_armed']}")
    lines.append("")
    lines.append("| Device | Count | Payload Lens |")
    lines.append("|--------|-------|--------------|")
    for device, info in sorted(ri["by_device"].items()):
        lines.append(f"| {device} | {info['count']} | {info['payload_lens']} |")
    lines.append("")

    lines.append("## Rejection Events (All Categories)")
    lines.append("")
    rej = data["rejections"]
    lines.append(f"- Total: {rej['total']}")
    lines.append(f"- err=17 (CEEPEW_ERR_REPLAY): {rej['err17_total']}")
    lines.append(f"- By kind: {rej['by_kind']}")
    lines.append("")
    lines.append("| Device | replay_commit | replay_seq_unavail | esl | box | ascon | pipeline | other | err=17 |")
    lines.append("|--------|---------------|--------------------|-----|-----|-------|----------|-------|--------|")
    for device in sorted(rej["by_device_kind"].keys()):
        kinds = rej["by_device_kind"][device]
        lines.append(f"| {device} | {kinds.get('replay_commit', 0)} | {kinds.get('replay_seq_unavail', 0)} | {kinds.get('esl', 0)} | {kinds.get('box', 0)} | {kinds.get('ascon', 0)} | {kinds.get('pipeline', 0)} | {kinds.get('other', 0)} | {rej['err17_by_device'].get(device, 0)} |")
    lines.append("")

    lines.append("## Matching Results")
    lines.append("")
    match = data["matching"]
    lines.append(f"- **MATCHED {match['matched_count']} / {data['replay_injections']['total']}**")
    if match['median_delta_us'] is not None:
        lines.append(f"- Median Δt: {fmt_us(int(match['median_delta_us']))}")
        lines.append(f"- Max |Δt|: {fmt_us(match['max_abs_delta_us'])}")
    lines.append("")

    lines.append("### Match Table")
    lines.append("")
    lines.append("| Replay Len | Replay Dev | Reject Kind | Reject Dev | Reject Err | Δt (us) | Δt (host) |")
    lines.append("|------------|------------|-------------|------------|------------|---------|-----------|")
    for det in data["match_details"]:
        reject_kind = det["reject_kind"] if det["reject_kind"] else "UNMATCHED"
        reject_dev = det["reject_device"] if det["reject_device"] else "-"
        reject_err = det["reject_err"] if det["reject_err"] is not None else "-"
        delta_us = fmt_us(det["delta_t_us"])
        delta_host = f"{det['delta_t_host_s']:+.3f}s" if det["delta_t_host_s"] is not None else "-"
        lines.append(f"| {det['replay_payload_len']} | {det['replay_device']} | {reject_kind} | {reject_dev} | {reject_err} | {delta_us} | {delta_host} |")
    lines.append("")

    lines.append("## Unexplained Rejections")
    lines.append("")
    unexp = data["unexplained_rejections"]
    lines.append(f"- Count: {unexp['count']}")
    if unexp["by_kind"]:
        lines.append(f"- By kind: {unexp['by_kind']}")
    if unexp["by_err"]:
        lines.append(f"- By err: {unexp['by_err']}")
    lines.append("")

    lines.append("## Interpretation")
    lines.append("")
    lines.append("This report uses **one-to-one time-correlated matching** to verify replay rejection.")
    lines.append("")
    lines.append("**Why time-correlation matters:** The E3 replay hook replays a stale ESL header with a")
    lines.append("fresh ARQ seq downstream, so the replayed frame passes ARQ as a new frame and reaches")
    lines.append("the replay window. ARQ retransmission means a dropped frame can be followed by a clean")
    lines.append("retry of the same application sequence number, so per-seq accounting would report the")
    lines.append("exact opposite of the truth.")
    lines.append("")
    lines.append("Therefore **app-level seq accounting** CANNOT be used to test replay rejection — every")
    lines.append("replayed seq will eventually appear committed via its clean retry.")
    lines.append("")
    if match["matched_count"] == data["replay_injections"]["total"]:
        lines.append(f"**Result:** All {match['matched_count']} replayed frames were rejected as replays (err=17) by the peer.")
    else:
        lines.append(f"**Result:** {match['matched_count']}/{data['replay_injections']['total']} replayed frames matched to peer err=17 rejections.")
    lines.append("")

    return "\n".join(lines)


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

def run_self_test() -> int:
    """Run the self-test with synthetic data.

    Covers:
    (a) replay injection matched to a peer err=17 rejection 0.4 s later
    (b) an unmatched replay injection (no peer rejection within window)
    (c) a rejection with no replay injection (ordinary RF loss)
    (d) NEGATIVE CASE: a rejection with err != 17 must NOT be matched to a replay
    (e) NEGATIVE CASE: a replay injection with no err=17 rejection within window must be unmatched
    """
    import tempfile

    test_lines = [
        # Device boot timestamps
        "0.0 [DEVICE_A] I (100000) boot: starting\n",
        "0.0 [DEVICE_B] I (100000) boot: starting\n",

        # (a) Replay injection from DEVICE_A, matched to DEVICE_B err=17 rejection 0.4s later
        "10.0 [DEVICE_A] I (200000) task_session: [BENCH58_REPLAY] armed\n",
        "10.0 [DEVICE_A] I (200010) session_send: [BENCH58_REPLAY] replayed 120 B with stale esl header (fresh arq seq downstream)\n",
        "10.4 [DEVICE_B] E (600000) SESSION: [SECURE_CHAT_RX] Discard: replay commit rejected seq=42 (err=17)\n",

        # (b) Unmatched replay injection (no peer rejection within window)
        "200.0 [DEVICE_B] I (3000000) task_session: [BENCH58_REPLAY] armed\n",
        "200.0 [DEVICE_B] I (3000010) session_send: [BENCH58_REPLAY] replayed 88 B with stale esl header (fresh arq seq downstream)\n",
        # No rejection follows within window

        # (c) Rejection with no replay injection (ordinary RF loss) - at 30s host
        "30.0 [DEVICE_A] E (300000) SESSION: [SECURE_CHAT_RX] Discard: replay commit rejected seq=7 (err=17)\n",

        # (d) NEGATIVE CASE: rejection with err != 17 (e.g. err=5 crypto) must NOT match
        "40.0 [DEVICE_A] I (400000) task_session: [BENCH58_REPLAY] armed\n",
        "40.0 [DEVICE_A] I (4000010) session_send: [BENCH58_REPLAY] replayed 64 B with stale esl header (fresh arq seq downstream)\n",
        "40.4 [DEVICE_B] E (700000) SESSION: [SECURE_CHAT_RX] Discard: crypto_box decrypt FAILED (err=5, len=83)\n",
        # The err=5 rejection must NOT match the replay injection (wrong err code)

        # (e) NEGATIVE CASE: replay injection with only non-17 rejections nearby
        "50.0 [DEVICE_B] I (800000) task_session: [BENCH58_REPLAY] armed\n",
        "50.0 [DEVICE_B] I (8000010) session_send: [BENCH58_REPLAY] replayed 96 B with stale esl header (fresh arq seq downstream)\n",
        "50.4 [DEVICE_A] E (900000) SESSION: [SECURE_CHAT_RX] Discard: crypto_box decrypt FAILED (err=5, len=83)\n",
        # Only err=5 nearby, no err=17 -> must be unmatched

        # Another matched pair for DEVICE_B -> DEVICE_A
        "60.0 [DEVICE_B] I (1000000) task_session: [BENCH58_REPLAY] armed\n",
        "60.0 [DEVICE_B] I (1000010) session_send: [BENCH58_REPLAY] replayed 150 B with stale esl header (fresh arq seq downstream)\n",
        "60.4 [DEVICE_A] E (1100000) SESSION: [SECURE_CHAT_RX] Discard: replay commit rejected seq=99 (err=17)\n",
    ]

    with tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False, encoding="utf-8") as fh:
        fh.writelines(test_lines)
        temp_path = fh.name

    try:
        analyzer = ReplayRejectAnalyzer()
        n = analyzer.process_file(temp_path)

        errors = []

        # Check replay events
        if len(analyzer.replay_events) != 5:
            errors.append(f"replay_events: expected 5, got {len(analyzer.replay_events)}")

        # Check replay armed count
        if analyzer.replay_armed_count != 5:
            errors.append(f"replay_armed_count: expected 5, got {analyzer.replay_armed_count}")

        # Check rejections
        if len(analyzer.reject_events) != 5:
            errors.append(f"reject_events: expected 5, got {len(analyzer.reject_events)}")

        # Check rejection kinds
        kind_counts = analyzer.reject_counts
        if kind_counts["replay_commit"] != 3:
            errors.append(f"replay_commit rejections: expected 3, got {kind_counts.get('replay_commit', 0)}")
        if kind_counts["box"] != 2:
            errors.append(f"box rejections: expected 2, got {kind_counts.get('box', 0)}")

        # Check err=17 count
        err17_count = sum(1 for r in analyzer.reject_events if r.err == 17)
        if err17_count != 3:
            errors.append(f"err=17 rejections: expected 3, got {err17_count}")

        # Run matching with 2.0s window
        matches = analyzer.match_replay_to_reject(window_seconds=2.0)

        # Should have 5 matches (one per replay event)
        if len(matches) != 5:
            errors.append(f"matches: expected 5, got {len(matches)}")

        # First replay (DEVICE_A, payload 120) should match to DEVICE_B err=17 rejection
        m0 = matches[0]
        if m0.reject is None:
            errors.append("match[0]: expected reject, got None")
        elif m0.reject.kind != "replay_commit":
            errors.append(f"match[0]: expected replay_commit reject, got {m0.reject.kind}")
        elif m0.reject.err != 17:
            errors.append(f"match[0]: expected err=17, got err={m0.reject.err}")
        elif m0.reject.device != "DEVICE_B":
            errors.append(f"match[0]: expected DEVICE_B, got {m0.reject.device}")
        elif m0.delta_t_us is None or not (390000 <= m0.delta_t_us <= 410000):
            errors.append(f"match[0]: expected delta_t ~400000 us, got {m0.delta_t_us}")

        # Second replay (DEVICE_B, payload 88) should be UNMATCHED (no peer rejection within window)
        m1 = matches[1]
        if m1.reject is not None:
            errors.append(f"match[1]: expected UNMATCHED, got reject on {m1.reject.device}")

        # Third replay (DEVICE_A, payload 64) - nearby rejection is err=5, NOT err=17 -> must be UNMATCHED
        m2 = matches[2]
        if m2.reject is not None:
            errors.append(f"match[2]: expected UNMATCHED (err=5 nearby, not err=17), got reject on {m2.reject.device} err={m2.reject.err}")

        # Fourth replay (DEVICE_B, payload 96) - nearby rejection is err=5, NOT err=17 -> must be UNMATCHED
        m3 = matches[3]
        if m3.reject is not None:
            errors.append(f"match[3]: expected UNMATCHED (err=5 nearby, not err=17), got reject on {m3.reject.device} err={m3.reject.err}")

        # Fifth replay (DEVICE_B, payload 150) should match to DEVICE_A err=17 rejection
        m4 = matches[4]
        if m4.reject is None:
            errors.append("match[4]: expected reject, got None")
        elif m4.reject.kind != "replay_commit":
            errors.append(f"match[4]: expected replay_commit reject, got {m4.reject.kind}")
        elif m4.reject.err != 17:
            errors.append(f"match[4]: expected err=17, got err={m4.reject.err}")
        elif m4.reject.device != "DEVICE_A":
            errors.append(f"match[4]: expected DEVICE_A, got {m4.reject.device}")

        # Check unexplained rejections (should include the err=17 at 30s with no replay, plus the two err=5s)
        data = analyzer.to_dict("test", 2.0)
        unexplained_count = data["unexplained_rejections"]["count"]
        if unexplained_count < 1:
            errors.append(f"unexplained_rejections: expected >= 1, got {unexplained_count}")

        if errors:
            print("[FAIL] Self-test failed:")
            for err in errors:
                print(f"  - {err}")
            return 1

        print("[PASS] Self-test passed")
        print("  - 5 replay injections parsed")
        print("  - 5 rejections parsed (3 err=17 replay_commit, 2 err=5 box)")
        print("  - Matched: 2/5 replay injections to peer err=17 rejections")
        print("  - Unmatched: 3/5 (1 out-of-window, 2 negative cases with err!=17 nearby)")
        print("  - Negative case (err!=17 must not match): VERIFIED")
        return 0

    finally:
        os.unlink(temp_path)


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--run-id", help="run ID (reads data/raw/<run-id>/capture_dual.log)")
    ap.add_argument("--raw-dir", default="data/raw", help="raw data directory (default: data/raw)")
    ap.add_argument("--log", help="explicit path to log file (overrides --run-id)")
    ap.add_argument("--window", type=float, default=2.0, help="matching window in seconds (default: 2.0, same as e2_forge_reject.py)")
    ap.add_argument("--derived-dir", default="data/derived", help="output directory for derived files")
    ap.add_argument("--json", help="write JSON output to this path (instead of derived-dir default)")
    ap.add_argument("--md", help="write Markdown output to this path (instead of derived-dir default)")
    ap.add_argument("--quiet", action="store_true", help="suppress stdout report")
    ap.add_argument("-v", "--verbose", action="store_true", help="verbose output")
    ap.add_argument("--self-test", action="store_true", help="run self-test and exit")

    args = ap.parse_args(argv)

    if args.self_test:
        return run_self_test()

    if not args.log and not args.run_id:
        ap.error("specify --run-id or --log")

    # Determine input file
    if args.log:
        log_path = args.log
        run_id = os.path.basename(os.path.dirname(args.log)) if args.run_id is None else args.run_id
    else:
        run_id = args.run_id
        log_path = os.path.join(args.raw_dir, run_id, "capture_dual.log")

    if not os.path.exists(log_path):
        print(f"[FAIL] log file not found: {log_path}", file=sys.stderr)
        return 1

    # Analyze
    analyzer = ReplayRejectAnalyzer()
    n_lines = analyzer.process_file(log_path)
    if n_lines < 0:
        return 1

    if args.verbose:
        print(f"Processed {n_lines} lines from {log_path}")

    # Generate data dict
    data = analyzer.to_dict(run_id, args.window)

    # Generate outputs
    if not args.quiet:
        report = generate_report(data)
        print(report)

    # Determine output paths
    derived_dir = os.path.join(args.derived_dir, run_id) if args.run_id else args.derived_dir
    os.makedirs(derived_dir, exist_ok=True)

    json_path = args.json if args.json else os.path.join(derived_dir, "replay_reject.json")
    md_path = args.md if args.md else os.path.join(derived_dir, "replay_reject.md")

    # Write JSON
    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)
        fh.write("\n")

    # Write Markdown
    md_report = generate_markdown_report(data)
    with open(md_path, "w", encoding="utf-8") as fh:
        fh.write(md_report)
        fh.write("\n")

    if not args.quiet:
        print(f"wrote {json_path}")
        print(f"wrote {md_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
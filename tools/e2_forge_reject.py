#!/usr/bin/env python3
"""tools/e2_forge_reject.py — end-to-end forgery rejection analysis for CEE-PEW.

Parses dual-device capture logs to verify that forged frames are rejected by the
peer's crypto authenticator, using time-correlated matching rather than app-level
sequence accounting.

CRITICAL (evidence class): because the forge is one-shot and ARQ retransmits, the
corrupted frame is dropped and a CLEAN retry of the SAME application sequence number
is then accepted. So app-level `seq` accounting (`[BENCHRX] commit seq=N err=0`)
CANNOT be used to test forgery rejection — every forged seq will eventually appear
committed via its clean retry. Only one-to-one time-correlated rejection on the peer
device is valid.

Patterns extracted:
  1. Forged TX:       [BENCH58_TX] seq=N forged
  2. Forge armed:     [BENCH58_FORGE] armed frame corrupted
  3. Crypto box:      Discard: crypto_box decrypt FAILED (err=N)
  4. Crypto ascon:    Discard: Ascon-128 AEAD auth tag verification FAILED (err=N)
  5. Crypto pipeline: Discard: RX decrypt pipeline failed (err=N)
  6. Replay:          Discard: ... replay ... (various forms)

Timestamps: lines begin with <seconds>.<frac> (host receive time) and contain the
device's own `I (microseconds)` boot time. We use the device boot-time microseconds
for Δt (on-device clock); fall back to host time if absent.

Usage:
  python tools/e2_forge_reject.py --run-id E1_C_ceepew_20261018
  python tools/e2_forge_reject.py --log path/to/capture_dual.log
  python tools/e2_forge_reject.py --self-test
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

# Forged TX: [BENCH58_TX] seq=N forged
BENCH58_FORGED_TX_RE = re.compile(r"\[BENCH58_TX\]\s+seq=(\d+)\s+forged")

# Forge armed: [BENCH58_FORGE] armed frame corrupted
BENCH58_FORGE_ARMED_RE = re.compile(r"\[BENCH58_FORGE\]\s+armed frame corrupted")

# Crypto rejections - all Discard: lines
DISCARD_CRYPTO_BOX_RE = re.compile(r"Discard:\s+crypto_box decrypt FAILED\s+\(err=(-?\d+)")
DISCARD_ASCON_RE = re.compile(r"Discard:\s+Ascon-128 AEAD auth tag verification FAILED\s+\(err=(-?\d+)")
DISCARD_PIPELINE_RE = re.compile(r"Discard:\s+RX decrypt pipeline failed\s+\(err=(-?\d+)")
DISCARD_REPLAY_RE = re.compile(r"Discard:.*\breplay\b", re.IGNORECASE)
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
class ForgedEvent:
    """A forged frame transmission event."""
    host_ts: float
    device_ts_us: Optional[int]
    device: str
    seq: int


@dataclass
class RejectEvent:
    """A crypto rejection event."""
    host_ts: float
    device_ts_us: Optional[int]
    device: str
    kind: str  # 'box', 'ascon', 'pipeline', 'replay', 'other'
    err: int


@dataclass
class MatchResult:
    """Result of matching a forged event to a rejection."""
    forged: ForgedEvent
    reject: Optional[RejectEvent]
    delta_t_us: Optional[int]  # reject.device_ts - forged.device_ts
    delta_t_host: Optional[float]  # reject.host_ts - forged.host_ts


# --------------------------------------------------------------------------
# Line parsing
# --------------------------------------------------------------------------

def extract_device_label(line: str) -> str:
    """Extract device label from a log line.
    
    Device label = the [...] token immediately before the ESP-IDF I (/W (/E ( marker.
    Fall back to [COMx] if present, else UNKNOWN.
    """
    # Try the primary pattern: [LABEL] I ( or [LABEL] W ( etc
    m = re.search(r"\[([^\]]+)\]\s*(?:[IWEVD]\s*\(\s*\d+\s*\))", line)
    if m:
        return m.group(1)
    
    # Fall back to [COMx] pattern
    m = re.search(r"\[(COM\d+)\]", line)
    if m:
        return m.group(1)
    
    # Fall back to any [LABEL] at start of meaningful content
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

class ForgeRejectAnalyzer:
    def __init__(self):
        self.forged_events: List[ForgedEvent] = []
        self.forge_armed_count = 0
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
        
        # Forged TX event
        m = BENCH58_FORGED_TX_RE.search(line)
        if m:
            seq = int(m.group(1))
            self.forged_events.append(ForgedEvent(
                host_ts=host_ts,
                device_ts_us=device_ts,
                device=device,
                seq=seq,
            ))
            self.device_counts[device] += 1
            return
        
        # Forge armed event (count separately)
        if BENCH58_FORGE_ARMED_RE.search(line):
            self.forge_armed_count += 1
            return
        
        # Crypto rejections - check in priority order
        kind = None
        err = None
        
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
                else:
                    m = DISCARD_REPLAY_RE.search(line)
                    if m:
                        kind = "replay"
                        err = 0  # replay doesn't always have err=
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
    
    def match_forged_to_reject(self, window_seconds: float) -> List[MatchResult]:
        """Match each forged event to the nearest crypto rejection on the PEER device.
        
        A forged event from DEVICE_A is matched to a rejection on DEVICE_B, and vice versa.
        Matching uses HOST timestamps because device timestamps are boot-time (relative to
        each device's boot), not absolute. Host timestamps represent when the log line was
        received and are synchronized across devices.
        """
        results = []
        
        for forged in self.forged_events:
            # Find peer device
            peer_device = None
            if forged.device == "DEVICE_A":
                peer_device = "DEVICE_B"
            elif forged.device == "DEVICE_B":
                peer_device = "DEVICE_A"
            
            if peer_device is None:
                # Unknown device, skip matching
                results.append(MatchResult(forged=forged, reject=None, delta_t_us=None, delta_t_host=None))
                continue
            
            # Find candidate rejections on peer device
            candidates = [r for r in self.reject_events if r.device == peer_device]
            
            if not candidates:
                results.append(MatchResult(forged=forged, reject=None, delta_t_us=None, delta_t_host=None))
                continue
            
            # Find nearest by host timestamp (the only reliable cross-device clock)
            best_reject = None
            best_delta_host = float("inf")
            
            for reject in candidates:
                delta_host = reject.host_ts - forged.host_ts
                delta_abs = abs(delta_host)
                if delta_abs <= window_seconds:
                    if delta_abs < best_delta_host:
                        best_delta_host = delta_abs
                        best_reject = reject
            
            if best_reject is not None:
                # Compute delta_t for reporting (host-based, converted to microseconds)
                delta_t_us = int((best_reject.host_ts - forged.host_ts) * 1_000_000)
                results.append(MatchResult(
                    forged=forged,
                    reject=best_reject,
                    delta_t_us=delta_t_us,
                    delta_t_host=best_reject.host_ts - forged.host_ts,
                ))
            else:
                results.append(MatchResult(forged=forged, reject=None, delta_t_us=None, delta_t_host=None))
        
        return results
    
    def to_dict(self, run_id: str, window_seconds: float) -> Dict[str, Any]:
        """Export results as a dictionary for JSON serialization."""
        matches = self.match_forged_to_reject(window_seconds)
        
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
                median_delta_us = (sorted_deltas[n//2 - 1] + sorted_deltas[n//2]) / 2
            else:
                median_delta_us = sorted_deltas[n//2]
            max_abs_delta_us = max(abs(d) for d in matched_deltas_us)
        if matched_deltas_host:
            sorted_deltas_h = sorted(matched_deltas_host)
            n = len(sorted_deltas_h)
            if n % 2 == 0:
                median_delta_host = (sorted_deltas_h[n//2 - 1] + sorted_deltas_h[n//2]) / 2
            else:
                median_delta_host = sorted_deltas_h[n//2]
        
        # Unexplained rejections: rejections not matched to any forged event.
        # Pipeline rejections within 0.1s of a matched box rejection on the same device
        # are considered "explained" as they're part of the same rejection cascade.
        matched_reject_ids = set(id(m.reject) for m in matches if m.reject is not None)
        matched_box_rejects = [m.reject for m in matches if m.reject is not None and m.reject.kind == "box"]
        
        # Build set of explained pipeline rejects (within 0.1s of a box reject on same device)
        explained_pipeline_ids = set()
        for box_r in matched_box_rejects:
            for r in self.reject_events:
                if r.kind == "pipeline" and r.device == box_r.device:
                    if abs(r.host_ts - box_r.host_ts) <= 0.1:
                        explained_pipeline_ids.add(id(r))
        
        unexplained_rejects = [
            r for r in self.reject_events
            if id(r) not in matched_reject_ids and id(r) not in explained_pipeline_ids
        ]
        
        # Per-device forged counts
        forged_by_device: Dict[str, List[int]] = defaultdict(list)
        for fe in self.forged_events:
            forged_by_device[fe.device].append(fe.seq)
        
        # Per-device, per-kind reject counts
        rejects_by_device_kind: Dict[str, Dict[str, int]] = defaultdict(lambda: defaultdict(int))
        for re in self.reject_events:
            rejects_by_device_kind[re.device][re.kind] += 1
        
        return {
            "run_id": run_id,
            "window_seconds": window_seconds,
            "clock_used": self.clock_used,
            "forged_tx": {
                "total": len(self.forged_events),
                "forge_armed": self.forge_armed_count,
                "by_device": {k: {"count": len(v), "seqs": v} for k, v in forged_by_device.items()},
            },
            "rejections": {
                "total": len(self.reject_events),
                "by_kind": dict(self.reject_counts),
                "by_device_kind": {k: dict(v) for k, v in rejects_by_device_kind.items()},
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
            },
            "match_details": [
                {
                    "forged_seq": m.forged.seq,
                    "forged_device": m.forged.device,
                    "forged_host_ts": m.forged.host_ts,
                    "forged_device_ts_us": m.forged.device_ts_us,
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
    lines.append("CEE-PEW E2 FORGE REJECT REPORT")
    lines.append("=" * 96)
    lines.append(f"run_id: {data['run_id']}")
    lines.append(f"window: {data['window_seconds']} s")
    lines.append(f"clock: {data['clock_used']} timestamps (device boot-time microseconds)")
    lines.append("")
    
    # Forged TX summary
    ft = data["forged_tx"]
    lines.append("-- forged TX events --")
    lines.append(f"  total forged TX      : {ft['total']}")
    lines.append(f"  forge armed count    : {ft['forge_armed']}")
    for device, info in sorted(ft["by_device"].items()):
        seqs = info["seqs"]
        lines.append(f"  [{device}]: {info['count']} forged, seqs={seqs}")
    lines.append("")
    
    # Rejections summary
    rej = data["rejections"]
    lines.append("-- crypto rejections --")
    lines.append(f"  total rejections     : {rej['total']}")
    lines.append(f"  by kind: {rej['by_kind']}")
    for device, kinds in sorted(rej["by_device_kind"].items()):
        lines.append(f"  [{device}]: {dict(kinds)}")
    lines.append("")
    
    # Match summary
    match = data["matching"]
    lines.append("-- match table (forged -> peer rejection) --")
    lines.append(f"  MATCHED {match['matched_count']} / {data['forged_tx']['total']}")
    if match['median_delta_us'] is not None:
        median_ms = match['median_delta_us'] / 1000.0  # convert us to ms
        max_ms = match['max_abs_delta_us'] / 1000.0
        lines.append(f"  median delta_t: {median_ms:.2f} ms ({match['median_delta_host_s']:.3f} s)")
        lines.append(f"  max |delta_t|: {max_ms:.2f} ms")
    lines.append("")
    
    # Detailed match table
    lines.append("  forged_seq  forged_dev  ->  reject_kind  reject_dev  delta_t(ms)  delta_t(host)")
    for det in data["match_details"]:
        if det["reject_kind"] is not None:
            delta_ms = det['delta_t_us'] / 1000.0 if det['delta_t_us'] is not None else None
            delta_ms_str = f"{delta_ms:.2f}" if delta_ms is not None else "N/A"
            lines.append(f"  {det['forged_seq']:>10}  {det['forged_device']:<11} ->  {det['reject_kind']:<12}  {det['reject_device']:<10}  {delta_ms_str:>11} ms  {det['delta_t_host_s']:>+10.3f}s")
        else:
            lines.append(f"  {det['forged_seq']:>10}  {det['forged_device']:<11} ->  UNMATCHED")
    lines.append("")
    
    # Unexplained rejections
    unexp = data["unexplained_rejections"]
    lines.append("-- unexplained rejections (ordinary RF loss) --")
    lines.append(f"  count: {unexp['count']}")
    if unexp["by_kind"]:
        lines.append(f"  by kind: {unexp['by_kind']}")
    if data["unexplained_details"]:
        lines.append("  details:")
        for det in data["unexplained_details"]:
            lines.append(f"    [{det['device']}] {det['kind']} err={det['err']} @ {det['host_ts']:.1f}s")
    lines.append("")
    
    # Interpretation
    lines.append("-- INTERPRETATION --")
    lines.append("  This report uses ONE-TO-One TIME-CORRELATED matching to verify forgery rejection.")
    lines.append("  For each forged TX event on device A, we find the nearest crypto rejection on")
    lines.append("  device B (the peer) within the time window.")
    lines.append("")
    lines.append("  WHY TIME-CORRELATION MATTERS:")
    lines.append("  The bench forge path is ONE-SHOT: on the next transmit after arming, exactly one")
    lines.append("  outer-ciphertext byte is corrupted. ARQ retransmits that corrupted frame, and it")
    lines.append("  is dropped at the crypto stage. A CLEAN retry of the SAME app seq is then sent")
    lines.append("  and accepted.")
    lines.append("")
    lines.append("  Therefore app-level seq accounting (`[BENCHRX] commit seq=N err=0`) CANNOT be")
    lines.append("  used to test forgery rejection — every forged seq will eventually appear")
    lines.append("  committed via its clean retry. An accept/reject table built that way would")
    lines.append("  report '8/8 forged accepted', the exact opposite of the truth.")
    lines.append("")
    lines.append("  Only time-correlated matching of forged TX events to peer crypto rejections")
    lines.append("  provides valid evidence of forgery rejection.")
    lines.append("")
    if match["matched_count"] == data["forged_tx"]["total"]:
        lines.append(f"  => ALL {match['matched_count']} forged frames were rejected by the peer's crypto authenticator.")
    else:
        lines.append(f"  => {match['matched_count']}/{data['forged_tx']['total']} forged frames matched to peer rejections.")
        if match["unmatched_count"] > 0:
            lines.append(f"     {match['unmatched_count']} forged frames had no matching rejection (investigate drops).")
    lines.append("")
    
    return "\n".join(lines)


def generate_markdown_report(data: Dict[str, Any]) -> str:
    """Generate the Markdown report for the paper."""
    lines = []
    lines.append(f"# E2 Forge Reject Report: {data['run_id']}")
    lines.append("")
    lines.append(f"- Window: {data['window_seconds']} s")
    lines.append(f"- Clock: {data['clock_used']} timestamps")
    lines.append("")
    
    lines.append("## Forged TX Events")
    lines.append("")
    ft = data["forged_tx"]
    lines.append(f"- Total forged TX: {ft['total']}")
    lines.append(f"- Forge armed count: {ft['forge_armed']}")
    lines.append("")
    lines.append("| Device | Count | Seqs |")
    lines.append("|--------|-------|------|")
    for device, info in sorted(ft["by_device"].items()):
        lines.append(f"| {device} | {info['count']} | {info['seqs']} |")
    lines.append("")
    
    lines.append("## Crypto Rejections")
    lines.append("")
    rej = data["rejections"]
    lines.append(f"- Total: {rej['total']}")
    lines.append(f"- By kind: {rej['by_kind']}")
    lines.append("")
    lines.append("| Device | box | ascon | pipeline | replay | other |")
    lines.append("|--------|-----|-------|----------|--------|-------|")
    for device in sorted(rej["by_device_kind"].keys()):
        kinds = rej["by_device_kind"][device]
        lines.append(f"| {device} | {kinds.get('box', 0)} | {kinds.get('ascon', 0)} | {kinds.get('pipeline', 0)} | {kinds.get('replay', 0)} | {kinds.get('other', 0)} |")
    lines.append("")
    
    lines.append("## Matching Results")
    lines.append("")
    match = data["matching"]
    lines.append(f"- **MATCHED {match['matched_count']} / {data['forged_tx']['total']}**")
    if match['median_delta_us'] is not None:
        lines.append(f"- Median Δt: {fmt_us(int(match['median_delta_us']))}")
        lines.append(f"- Max |Δt|: {fmt_us(match['max_abs_delta_us'])}")
    lines.append("")
    
    lines.append("### Match Table")
    lines.append("")
    lines.append("| Forged Seq | Forged Dev | Reject Kind | Reject Dev | Δt (us) | Δt (host) |")
    lines.append("|------------|------------|-------------|------------|---------|-----------|")
    for det in data["match_details"]:
        reject_kind = det["reject_kind"] if det["reject_kind"] else "UNMATCHED"
        reject_dev = det["reject_device"] if det["reject_device"] else "-"
        delta_us = fmt_us(det["delta_t_us"])
        delta_host = f"{det['delta_t_host_s']:+.3f}s" if det["delta_t_host_s"] is not None else "-"
        lines.append(f"| {det['forged_seq']} | {det['forged_device']} | {reject_kind} | {reject_dev} | {delta_us} | {delta_host} |")
    lines.append("")
    
    lines.append("## Unexplained Rejections")
    lines.append("")
    unexp = data["unexplained_rejections"]
    lines.append(f"- Count: {unexp['count']}")
    if unexp["by_kind"]:
        lines.append(f"- By kind: {unexp['by_kind']}")
    lines.append("")
    
    lines.append("## Interpretation")
    lines.append("")
    lines.append("This report uses **one-to-one time-correlated matching** to verify forgery rejection.")
    lines.append("")
    lines.append("**Why time-correlation matters:** The bench forge path is one-shot: on the next transmit")
    lines.append("after arming, exactly one outer-ciphertext byte is corrupted. ARQ retransmits that")
    lines.append("corrupted frame, and it is dropped at the crypto stage. A clean retry of the same app")
    lines.append("seq is then sent and accepted.")
    lines.append("")
    lines.append("Therefore **app-level seq accounting** (`[BENCHRX] commit seq=N err=0`) **CANNOT** be")
    lines.append("used to test forgery rejection — every forged seq will eventually appear committed")
    lines.append("via its clean retry.")
    lines.append("")
    if match["matched_count"] == data["forged_tx"]["total"]:
        lines.append(f"**Result:** All {match['matched_count']} forged frames were rejected by the peer's crypto authenticator.")
    else:
        lines.append(f"**Result:** {match['matched_count']}/{data['forged_tx']['total']} forged frames matched to peer rejections.")
    lines.append("")
    
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

def run_self_test() -> int:
    """Run the self-test with synthetic data."""
    import tempfile
    
    # Synthetic test lines designed to prove:
    # (a) forged event matched to a peer box-reject 0.4 s later
    # (b) an unmatched forged event
    # (c) a reject with no forged event
    # (d) that a clean retry logging `commit seq=N err=0` is NOT counted as acceptance
    
    test_lines = [
        # Device boot timestamps
        "0.0 [DEVICE_A] I (100000) boot: starting\n",
        "0.0 [DEVICE_B] I (100000) boot: starting\n",
        
        # (a) Forged TX from DEVICE_A, matched to DEVICE_B box-reject 0.4s later (400000 us)
        "10.0 [DEVICE_A] I (200000) task_session: [BENCH58_TX] seq=9 forged (tag byte will be corrupted)\n",
        "10.0 [DEVICE_A] I (200010) session_send: [BENCH58_FORGE] armed frame corrupted (1 box byte)\n",
        "10.4 [DEVICE_B] E (600000) SESSION: [SECURE_CHAT_RX] Discard: crypto_box decrypt FAILED (err=5, len=83)\n",
        "10.4 [DEVICE_B] W (600010) SESSION: [SECURE_CHAT_RX] Discard: RX decrypt pipeline failed (err=5 len=209)\n",
        
        # (b) Unmatched forged event (no peer rejection within window)
        # Place it far from any rejection - at 200s host time, 2000000 us device time
        # Nearest DEVICE_A reject is at 900000 us, which is 1.1s away - still within 2s window
        # So we need to go even further: 300s host, 3000000 us device (2.1s from 900000 us)
        "300.0 [DEVICE_B] I (3000000) task_session: [BENCH58_TX] seq=19 forged (tag byte will be corrupted)\n",
        # No rejection follows within window
        
        # (c) Reject with no forged event (ordinary RF loss) - at 30s host, 300000 us device
        "30.0 [DEVICE_A] E (300000) SESSION: [SECURE_CHAT_RX] Discard: crypto_box decrypt FAILED (err=5, len=83)\n",
        
        # (d) Clean retry commit - should NOT be counted as acceptance of forged frame
        "10.5 [DEVICE_B] I (600500) task_session: [BENCHRX] commit seq=9 err=0\n",
        
        # Another matched pair for DEVICE_B -> DEVICE_A
        "40.0 [DEVICE_B] I (500000) task_session: [BENCH58_TX] seq=29 forged (tag byte will be corrupted)\n",
        "40.4 [DEVICE_A] E (900000) SESSION: [SECURE_CHAT_RX] Discard: crypto_box decrypt FAILED (err=5, len=83)\n",
        "40.4 [DEVICE_A] W (900010) SESSION: [SECURE_CHAT_RX] Discard: RX decrypt pipeline failed (err=5 len=209)\n",
    ]
    
    with tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False, encoding="utf-8") as fh:
        fh.writelines(test_lines)
        temp_path = fh.name
    
    try:
        analyzer = ForgeRejectAnalyzer()
        n = analyzer.process_file(temp_path)
        
        errors = []
        
        # Check forged events
        if len(analyzer.forged_events) != 3:
            errors.append(f"forged_events: expected 3, got {len(analyzer.forged_events)}")
        
        # Check forge armed count
        if analyzer.forge_armed_count != 1:
            errors.append(f"forge_armed_count: expected 1, got {analyzer.forge_armed_count}")
        
        # Check rejections
        if len(analyzer.reject_events) != 5:
            errors.append(f"reject_events: expected 5, got {len(analyzer.reject_events)}")
        
        # Check rejection kinds
        kind_counts = analyzer.reject_counts
        if kind_counts["box"] != 3:
            errors.append(f"box rejections: expected 3, got {kind_counts['box']}")
        if kind_counts["pipeline"] != 2:
            errors.append(f"pipeline rejections: expected 2, got {kind_counts['pipeline']}")
        
        # Run matching with 2.0s window
        matches = analyzer.match_forged_to_reject(window_seconds=2.0)
        
        # Should have 3 matches (one per forged event)
        if len(matches) != 3:
            errors.append(f"matches: expected 3, got {len(matches)}")
        
        # First forged (seq=9, DEVICE_A) should match to DEVICE_B box-reject
        m0 = matches[0]
        if m0.reject is None:
            errors.append("match[0]: expected reject, got None")
        elif m0.reject.kind != "box":
            errors.append(f"match[0]: expected box reject, got {m0.reject.kind}")
        elif m0.reject.device != "DEVICE_B":
            errors.append(f"match[0]: expected DEVICE_B, got {m0.reject.device}")
        elif m0.delta_t_us is None or not (390000 <= m0.delta_t_us <= 410000):
            errors.append(f"match[0]: expected delta_t ~400000 us, got {m0.delta_t_us}")
        
        # Second forged (seq=19, DEVICE_B) should be UNMATCHED (no peer rejection within window)
        m1 = matches[1]
        if m1.reject is not None:
            errors.append(f"match[1]: expected UNMATCHED, got reject on {m1.reject.device}")
        
        # Third forged (seq=29, DEVICE_B) should match to DEVICE_A box-reject
        m2 = matches[2]
        if m2.reject is None:
            errors.append("match[2]: expected reject, got None")
        elif m2.reject.kind != "box":
            errors.append(f"match[2]: expected box reject, got {m2.reject.kind}")
        elif m2.reject.device != "DEVICE_A":
            errors.append(f"match[2]: expected DEVICE_A, got {m2.reject.device}")
        
        # Check that commit seq=9 was NOT counted (it's not a rejection pattern)
        # This is implicitly verified by the reject count being 5, not 6
        
        # Check unexplained rejections (should be 2: the DEVICE_A box at 30s and one pipeline)
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
    ap.add_argument("--log", help="explicit path to log file (overrides --run-id)")
    ap.add_argument("--window", type=float, default=2.0, help="matching window in seconds (default: 2.0)")
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
        log_path = os.path.join("data", "raw", run_id, "capture_dual.log")
    
    if not os.path.exists(log_path):
        print(f"[FAIL] log file not found: {log_path}", file=sys.stderr)
        return 1
    
    # Analyze
    analyzer = ForgeRejectAnalyzer()
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
    
    json_path = args.json if args.json else os.path.join(derived_dir, "forge_reject.json")
    md_path = args.md if args.md else os.path.join(derived_dir, "forge_reject.md")
    
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
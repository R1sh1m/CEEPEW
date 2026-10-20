#!/usr/bin/env python3
"""tools/bench_latency_report.py — one-way over-the-air latency analysis for CEE-PEW E5 sync-pulse runs.

Parses dual-device capture logs to extract BENCH58 timing records and compute
same-clock intervals that represent true latency measurements.

CRITICAL (correctness rule): The two boards have INDEPENDENT microsecond clocks
(esp_timer_get_time() counts from each board's own boot; they are NOT synchronised).
Therefore:
  - NEVER compute a difference between timestamps originating on different devices.
  - Every reported statistic MUST be a same-clock interval:
      TX-side (single device): submit→pulse and pulse→sendcb
      RX-side (single device): edge→rxcb and rxcb→accept
  - edge→rxcb is the headline one-way figure: the sync pulse crosses a 20 cm wire
    in nanoseconds and the frame is handed to the radio immediately after the pulse,
    so this interval captures radio TX + propagation + RX + callback delivery, all
    measured on the receiver's own clock.
  - If a record has a zero timestamp for a field needed by an interval, skip that
    interval and count it under a "not measurable" bucket — never substitute another
    timestamp or a host timestamp.

Input format (one line per BENCH58 frame, may be interleaved with ESP-IDF logs):
  <host_ts>.<frac> [DEVICE] ... BENCH_TIME seq=<n> role=<tx|rx> submit=<u64> pulse=<u64> sendcb=<u64> edge=<u64> rxcb=<u64> accept=<u64>

Fields that do not apply to that record's role are 0. Lines may be prefixed with
the monitor's host timestamp and device label (e.g. [COM3]). Parse defensively:
skip lines that do not match the full expected field set, count them, and report
how many records were recovered vs skipped.

Usage:
  python tools/bench_latency_report.py --run-id E5_sync_pulse_20261019
  python tools/bench_latency_report.py --log path/to/capture_dual.log
  python tools/bench_latency_report.py --self-test
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

# BENCH_TIME record: the full line must match all fields
# BENCH_TIME seq=<n> role=<tx|rx> submit=<u64> pulse=<u64> sendcb=<u64> edge=<u64> rxcb=<u64> accept=<u64>
BENCH_TIME_RE = re.compile(
    r"BENCH_TIME\s+"
    r"seq=(\d+)\s+"
    r"role=(tx|rx)\s+"
    r"submit=(\d+)\s+"
    r"pulse=(\d+)\s+"
    r"sendcb=(\d+)\s+"
    r"edge=(\d+)\s+"
    r"rxcb=(\d+)\s+"
    r"accept=(\d+)"
)


# --------------------------------------------------------------------------
# Data structures
# --------------------------------------------------------------------------

@dataclass
class BenchRecord:
    """A parsed BENCH_TIME record."""
    host_ts: float
    device_ts_us: Optional[int]
    device: str
    seq: int
    role: str  # 'tx' or 'rx'
    submit: int
    pulse: int
    sendcb: int
    edge: int
    rxcb: int
    accept: int


@dataclass
class IntervalStats:
    """Statistics for a single interval type."""
    values_us: List[int] = field(default_factory=list)
    
    def add(self, value_us: int) -> None:
        self.values_us.append(value_us)
    
    def compute(self) -> Dict[str, Any]:
        """Compute statistics: N, min, median, IQR, p95, max, mean."""
        if not self.values_us:
            return {
                "n": 0,
                "min_us": None, "max_us": None, "mean_us": None,
                "median_us": None, "q1_us": None, "q3_us": None, "p95_us": None,
                "min_ms": None, "max_ms": None, "mean_ms": None,
                "median_ms": None, "q1_ms": None, "q3_ms": None, "p95_ms": None,
            }
        
        sorted_vals = sorted(self.values_us)
        n = len(sorted_vals)
        
        def percentile(p: float) -> float:
            """Compute percentile using linear interpolation."""
            if n == 1:
                return float(sorted_vals[0])
            k = (n - 1) * p / 100.0
            f = int(k)
            c = f + 1 if f + 1 < n else f
            return sorted_vals[f] + (k - f) * (sorted_vals[c] - sorted_vals[f])
        
        min_v = sorted_vals[0]
        max_v = sorted_vals[-1]
        mean_v = sum(sorted_vals) / n
        median_v = percentile(50)
        q1_v = percentile(25)
        q3_v = percentile(75)
        p95_v = percentile(95)
        
        return {
            "n": n,
            "min_us": min_v, "max_us": max_v, "mean_us": mean_v,
            "median_us": median_v, "q1_us": q1_v, "q3_us": q3_v, "p95_us": p95_v,
            "min_ms": min_v / 1000.0, "max_ms": max_v / 1000.0, "mean_ms": mean_v / 1000.0,
            "median_ms": median_v / 1000.0, "q1_ms": q1_v / 1000.0, "q3_ms": q3_v / 1000.0, "p95_ms": p95_v / 1000.0,
        }


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

class LatencyAnalyzer:
    def __init__(self):
        self.records: List[BenchRecord] = []
        self.skipped_lines = 0
        self.total_lines = 0
        self.clock_used = "device"  # or "host" if device ts unavailable
        
        # Per-device, per-role records
        self.tx_records_by_device: Dict[str, List[BenchRecord]] = defaultdict(list)
        self.rx_records_by_device: Dict[str, List[BenchRecord]] = defaultdict(list)
        
        # Intervals (same-clock only)
        # TX intervals: submit->pulse, pulse->sendcb
        self.tx_submit_pulse: Dict[str, IntervalStats] = defaultdict(IntervalStats)
        self.tx_pulse_sendcb: Dict[str, IntervalStats] = defaultdict(IntervalStats)
        # RX intervals: edge->rxcb (headline one-way), rxcb->accept
        self.rx_edge_rxcb: Dict[str, IntervalStats] = defaultdict(IntervalStats)
        self.rx_rxcb_accept: Dict[str, IntervalStats] = defaultdict(IntervalStats)
        
        # Not measurable (zero timestamps)
        self.not_measurable: Dict[str, int] = defaultdict(int)  # key: "device:role:interval"
        
        # Lost sync pulses (edge=0 on RX records)
        self.lost_pulse_seqs: Dict[str, List[int]] = defaultdict(list)
        
        # Seq tracking
        self.tx_seqs_by_device: Dict[str, List[int]] = defaultdict(list)
        self.rx_seqs_by_device: Dict[str, List[int]] = defaultdict(list)
        self.seq_seen_twice: Dict[str, List[int]] = defaultdict(list)  # device -> seqs seen twice
    
    def process_line(self, line: str) -> None:
        """Process a single log line."""
        self.total_lines += 1
        
        host_ts = extract_host_timestamp(line)
        if host_ts is None:
            self.skipped_lines += 1
            return
        
        device = extract_device_label(line)
        device_ts = extract_device_timestamp_us(line)
        
        if device_ts is not None:
            self.clock_used = "device"
        
        # Try to match BENCH_TIME record
        m = BENCH_TIME_RE.search(line)
        if not m:
            self.skipped_lines += 1
            return
        
        seq = int(m.group(1))
        role = m.group(2)
        submit = int(m.group(3))
        pulse = int(m.group(4))
        sendcb = int(m.group(5))
        edge = int(m.group(6))
        rxcb = int(m.group(7))
        accept = int(m.group(8))
        
        record = BenchRecord(
            host_ts=host_ts,
            device_ts_us=device_ts,
            device=device,
            seq=seq,
            role=role,
            submit=submit,
            pulse=pulse,
            sendcb=sendcb,
            edge=edge,
            rxcb=rxcb,
            accept=accept,
        )
        
        self.records.append(record)
        
        if role == "tx":
            self.tx_records_by_device[device].append(record)
            self.tx_seqs_by_device[device].append(seq)
            
            # TX intervals (same-clock: all from TX device)
            # submit -> pulse
            if submit > 0 and pulse > 0:
                delta = pulse - submit
                if delta >= 0:
                    self.tx_submit_pulse[device].add(delta)
                else:
                    # Negative interval is impossible; count as not measurable
                    self.not_measurable[f"{device}:tx:submit->pulse"] += 1
            else:
                self.not_measurable[f"{device}:tx:submit->pulse"] += 1
            
            # pulse -> sendcb
            if pulse > 0 and sendcb > 0:
                delta = sendcb - pulse
                if delta >= 0:
                    self.tx_pulse_sendcb[device].add(delta)
                else:
                    self.not_measurable[f"{device}:tx:pulse->sendcb"] += 1
            else:
                self.not_measurable[f"{device}:tx:pulse->sendcb"] += 1
        
        elif role == "rx":
            self.rx_records_by_device[device].append(record)
            self.rx_seqs_by_device[device].append(seq)
            
            # Check for lost sync pulse (edge=0)
            if edge == 0:
                self.lost_pulse_seqs[device].append(seq)
                # Cannot compute edge->rxcb or rxcb->accept without edge
                self.not_measurable[f"{device}:rx:edge->rxcb"] += 1
                if rxcb > 0 and accept > 0:
                    # rxcb->accept is still measurable
                    delta = accept - rxcb
                    if delta >= 0:
                        self.rx_rxcb_accept[device].add(delta)
                    else:
                        self.not_measurable[f"{device}:rx:rxcb->accept"] += 1
                else:
                    self.not_measurable[f"{device}:rx:rxcb->accept"] += 1
            else:
                # RX intervals (same-clock: all from RX device)
                # edge -> rxcb (headline one-way latency)
                if edge > 0 and rxcb > 0:
                    delta = rxcb - edge
                    if delta >= 0:
                        self.rx_edge_rxcb[device].add(delta)
                    else:
                        self.not_measurable[f"{device}:rx:edge->rxcb"] += 1
                else:
                    self.not_measurable[f"{device}:rx:edge->rxcb"] += 1
                
                # rxcb -> accept
                if rxcb > 0 and accept > 0:
                    delta = accept - rxcb
                    if delta >= 0:
                        self.rx_rxcb_accept[device].add(delta)
                    else:
                        self.not_measurable[f"{device}:rx:rxcb->accept"] += 1
                else:
                    self.not_measurable[f"{device}:rx:rxcb->accept"] += 1
    
    def process_file(self, path: str) -> int:
        """Process an entire log file. Returns number of lines processed."""
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    self.process_line(line)
        except OSError as exc:
            print(f"[FAIL] {path}: {exc}", file=sys.stderr)
            return -1
        return self.total_lines
    
    def _seq_coverage(self, seqs: List[int]) -> Dict[str, Any]:
        """Compute sequence coverage: min, max, unique, gaps, duplicates."""
        if not seqs:
            return {"min": None, "max": None, "unique": 0, "gaps": [], "gap_count": 0, "duplicates": []}
        
        unique_seqs = sorted(set(seqs))
        min_seq = min(unique_seqs)
        max_seq = max(unique_seqs)
        expected = set(range(min_seq, max_seq + 1))
        gaps = sorted(expected - set(unique_seqs))
        
        # Find duplicates
        seen = set()
        duplicates = []
        for s in seqs:
            if s in seen:
                if s not in duplicates:
                    duplicates.append(s)
            seen.add(s)
        
        return {
            "min": min_seq,
            "max": max_seq,
            "unique": len(unique_seqs),
            "gaps": gaps,
            "gap_count": len(gaps),
            "duplicates": duplicates,
        }
    
    def to_dict(self, run_id: str) -> Dict[str, Any]:
        """Export results as a dictionary for JSON serialization."""
        # Compute interval stats per device
        tx_stats_by_device: Dict[str, Dict[str, Any]] = {}
        for device in self.tx_records_by_device.keys():
            tx_stats_by_device[device] = {
                "submit->pulse": self.tx_submit_pulse[device].compute(),
                "pulse->sendcb": self.tx_pulse_sendcb[device].compute(),
            }
        
        rx_stats_by_device: Dict[str, Dict[str, Any]] = {}
        for device in self.rx_records_by_device.keys():
            rx_stats_by_device[device] = {
                "edge->rxcb": self.rx_edge_rxcb[device].compute(),  # headline one-way
                "rxcb->accept": self.rx_rxcb_accept[device].compute(),
            }
        
        # Seq coverage per device
        tx_coverage_by_device: Dict[str, Dict[str, Any]] = {}
        for device in self.tx_seqs_by_device:
            tx_coverage_by_device[device] = self._seq_coverage(self.tx_seqs_by_device[device])
        
        rx_coverage_by_device: Dict[str, Dict[str, Any]] = {}
        for device in self.rx_seqs_by_device:
            rx_coverage_by_device[device] = self._seq_coverage(self.rx_seqs_by_device[device])
        
        # Find seqs seen twice (in either role)
        seq_seen_twice: Dict[str, List[int]] = {}
        for device, seqs in self.tx_seqs_by_device.items():
            seen = set()
            twice = []
            for s in seqs:
                if s in seen:
                    twice.append(s)
                seen.add(s)
            if twice:
                seq_seen_twice[f"{device}:tx"] = twice
        
        for device, seqs in self.rx_seqs_by_device.items():
            seen = set()
            twice = []
            for s in seqs:
                if s in seen:
                    twice.append(s)
                seen.add(s)
            if twice:
                seq_seen_twice[f"{device}:rx"] = twice
        
        return {
            "run_id": run_id,
            "clock_used": self.clock_used,
            "parsing": {
                "total_lines": self.total_lines,
                "records_recovered": len(self.records),
                "lines_skipped": self.skipped_lines,
            },
            "intervals": {
                "tx_by_device": tx_stats_by_device,
                "rx_by_device": rx_stats_by_device,
            },
            "seq_coverage": {
                "tx_by_device": tx_coverage_by_device,
                "rx_by_device": rx_coverage_by_device,
            },
            "seq_seen_twice": seq_seen_twice,
            "lost_sync_pulses": {
                device: seqs for device, seqs in self.lost_pulse_seqs.items()
            },
            "not_measurable": dict(self.not_measurable),
            "interpretation": (
                "CRITICAL: The two boards have INDEPENDENT microsecond clocks "
                "(esp_timer_get_time() counts from each board's own boot; they are NOT synchronised). "
                "Therefore we NEVER compute a difference between timestamps originating on different devices. "
                "Every reported statistic is a same-clock interval: "
                "TX-side (single device): submit->pulse and pulse->sendcb. "
                "RX-side (single device): edge->rxcb (headline one-way latency) and rxcb->accept. "
                "edge->rxcb captures radio TX + propagation + RX + callback delivery, all measured on the receiver's own clock. "
                "If a record has a zero timestamp for a field needed by an interval, that interval is skipped "
                "and counted under 'not_measurable' - we never substitute another timestamp or a host timestamp."
            ),
        }


# --------------------------------------------------------------------------
# Report formatting
# --------------------------------------------------------------------------

def fmt_stats_table(stats: Dict[str, Any], label: str) -> List[str]:
    """Format interval statistics as a table row."""
    lines = []
    if stats["n"] == 0:
        lines.append(f"  {label}: N=0 (no data)")
        return lines
    
    lines.append(f"  {label}:")
    lines.append(f"    N={stats['n']}, min={stats['min_us']} us ({stats['min_ms']:.3f} ms), "
                 f"median={stats['median_us']} us ({stats['median_ms']:.3f} ms), "
                 f"p95={stats['p95_us']} us ({stats['p95_ms']:.3f} ms), "
                 f"max={stats['max_us']} us ({stats['max_ms']:.3f} ms), "
                 f"mean={stats['mean_us']:.1f} us ({stats['mean_ms']:.3f} ms)")
    lines.append(f"    IQR: q1={stats['q1_us']} us, q3={stats['q3_us']} us")
    return lines


def generate_report(data: Dict[str, Any]) -> str:
    """Generate the human-readable report."""
    lines = []
    lines.append("=" * 96)
    lines.append("CEE-PEW E5 SYNC-PULSE LATENCY REPORT")
    lines.append("=" * 96)
    lines.append(f"run_id: {data['run_id']}")
    lines.append(f"clock: {data['clock_used']} timestamps (device boot-time microseconds)")
    lines.append("")
    
    # Parsing summary
    p = data["parsing"]
    lines.append("-- parsing summary --")
    lines.append(f"  total lines scanned  : {p['total_lines']}")
    lines.append(f"  records recovered    : {p['records_recovered']}")
    lines.append(f"  lines skipped        : {p['lines_skipped']}")
    lines.append("")
    
    # TX intervals by device
    lines.append("-- TX intervals (same-clock: submit->pulse, pulse->sendcb) --")
    tx_data = data["intervals"]["tx_by_device"]
    if tx_data:
        for device, intervals in sorted(tx_data.items()):
            lines.append(f"  [{device}]:")
            for interval_name, stats in intervals.items():
                lines.extend(fmt_stats_table(stats, f"    {interval_name}"))
    else:
        lines.append("  (no TX records)")
    lines.append("")
    
    # RX intervals by device
    lines.append("-- RX intervals (same-clock: edge->rxcb [one-way], rxcb->accept) --")
    rx_data = data["intervals"]["rx_by_device"]
    if rx_data:
        for device, intervals in sorted(rx_data.items()):
            lines.append(f"  [{device}]:")
            for interval_name, stats in intervals.items():
                label = interval_name
                if interval_name == "edge->rxcb":
                    label = f"{interval_name} **ONE-WAY LATENCY**"
                lines.extend(fmt_stats_table(stats, f"    {label}"))
    else:
        lines.append("  (no RX records)")
    lines.append("")
    
    # Seq coverage
    lines.append("-- sequence coverage --")
    tx_cov = data["seq_coverage"]["tx_by_device"]
    rx_cov = data["seq_coverage"]["rx_by_device"]
    
    for device in sorted(set(list(tx_cov.keys()) + list(rx_cov.keys()))):
        lines.append(f"  [{device}]:")
        if device in tx_cov:
            c = tx_cov[device]
            if c["min"] is not None:
                lines.append(f"    TX: seq {c['min']}..{c['max']}, unique={c['unique']}, gaps={c['gap_count']}")
                if c["duplicates"]:
                    lines.append(f"         duplicates: {c['duplicates']}")
            else:
                lines.append(f"    TX: (no records)")
        if device in rx_cov:
            c = rx_cov[device]
            if c["min"] is not None:
                lines.append(f"    RX: seq {c['min']}..{c['max']}, unique={c['unique']}, gaps={c['gap_count']}")
                if c["duplicates"]:
                    lines.append(f"         duplicates: {c['duplicates']}")
            else:
                lines.append(f"    RX: (no records)")
    lines.append("")
    
    # Lost sync pulses
    lost = data["lost_sync_pulses"]
    lines.append("-- lost sync pulses (edge=0, no pulse observed) --")
    if lost:
        for device, seqs in sorted(lost.items()):
            lines.append(f"  [{device}]: {len(seqs)} lost, seqs={seqs}")
    else:
        lines.append("  (none)")
    lines.append("")
    
    # Not measurable
    nm = data["not_measurable"]
    lines.append("-- not measurable (zero timestamps) --")
    if nm:
        for key, count in sorted(nm.items()):
            lines.append(f"  {key}: {count}")
    else:
        lines.append("  (none)")
    lines.append("")
    
    # Interpretation
    lines.append("-- INTERPRETATION --")
    # Replace Unicode arrows with ASCII for console compatibility
    interp = data["interpretation"].replace("→", "->")
    lines.append(interp)
    lines.append("")
    
    # One-way latency summary
    lines.append("-- ONE-WAY LATENCY SUMMARY (edge->rxcb on receiver) --")
    one_way_found = False
    for device, intervals in sorted(rx_data.items()):
        stats = intervals.get("edge->rxcb", {})
        if stats.get("n", 0) > 0:
            one_way_found = True
            lines.append(f"  [{device}] (RX): median={stats['median_us']} us ({stats['median_ms']:.3f} ms), "
                         f"p95={stats['p95_us']} us ({stats['p95_ms']:.3f} ms), N={stats['n']}")
    if not one_way_found:
        lines.append("  (no one-way latency data available)")
    lines.append("")
    
    return "\n".join(lines)


def generate_markdown_report(data: Dict[str, Any]) -> str:
    """Generate the Markdown report for the paper."""
    lines = []
    lines.append(f"# E5 Sync-Pulse Latency Report: {data['run_id']}")
    lines.append("")
    lines.append(f"- Clock: {data['clock_used']} timestamps (device boot-time microseconds)")
    lines.append("")
    
    # Parsing summary
    p = data["parsing"]
    lines.append("## Parsing Summary")
    lines.append("")
    lines.append(f"- Total lines scanned: {p['total_lines']}")
    lines.append(f"- Records recovered: {p['records_recovered']}")
    lines.append(f"- Lines skipped: {p['lines_skipped']}")
    lines.append("")
    
    # TX intervals
    lines.append("## TX Intervals (Same-Clock)")
    lines.append("")
    tx_data = data["intervals"]["tx_by_device"]
    if tx_data:
        for device, intervals in sorted(tx_data.items()):
            lines.append(f"### {device} (TX)")
            lines.append("")
            lines.append("| Interval | N | Min (µs) | Median (µs) | P95 (µs) | Max (µs) | Mean (µs) |")
            lines.append("|----------|---|----------|-------------|----------|----------|-----------|")
            for interval_name, stats in intervals.items():
                if stats["n"] > 0:
                    lines.append(f"| {interval_name} | {stats['n']} | {stats['min_us']} | {stats['median_us']} | {stats['p95_us']} | {stats['max_us']} | {stats['mean_us']:.1f} |")
                else:
                    lines.append(f"| {interval_name} | 0 | - | - | - | - | - |")
            lines.append("")
    else:
        lines.append("*(no TX records)*")
        lines.append("")
    
    # RX intervals
    lines.append("## RX Intervals (Same-Clock)")
    lines.append("")
    rx_data = data["intervals"]["rx_by_device"]
    if rx_data:
        for device, intervals in sorted(rx_data.items()):
            lines.append(f"### {device} (RX)")
            lines.append("")
            lines.append("| Interval | N | Min (µs) | Median (µs) | P95 (µs) | Max (µs) | Mean (µs) |")
            lines.append("|----------|---|----------|-------------|----------|----------|-----------|")
            for interval_name, stats in intervals.items():
                label = f"**{interval_name}**" if interval_name == "edge->rxcb" else interval_name
                if stats["n"] > 0:
                    lines.append(f"| {label} | {stats['n']} | {stats['min_us']} | {stats['median_us']} | {stats['p95_us']} | {stats['max_us']} | {stats['mean_us']:.1f} |")
                else:
                    lines.append(f"| {label} | 0 | - | - | - | - | - |")
            lines.append("")
    else:
        lines.append("*(no RX records)*")
        lines.append("")
    
    # Seq coverage
    lines.append("## Sequence Coverage")
    lines.append("")
    tx_cov = data["seq_coverage"]["tx_by_device"]
    rx_cov = data["seq_coverage"]["rx_by_device"]
    
    lines.append("| Device | Role | Seq Range | Unique | Gaps | Duplicates |")
    lines.append("|--------|------|-----------|--------|------|------------|")
    for device in sorted(set(list(tx_cov.keys()) + list(rx_cov.keys()))):
        if device in tx_cov:
            c = tx_cov[device]
            range_str = f"{c['min']}..{c['max']}" if c['min'] is not None else "-"
            lines.append(f"| {device} | TX | {range_str} | {c['unique']} | {c['gap_count']} | {c['duplicates']} |")
        if device in rx_cov:
            c = rx_cov[device]
            range_str = f"{c['min']}..{c['max']}" if c['min'] is not None else "-"
            lines.append(f"| {device} | RX | {range_str} | {c['unique']} | {c['gap_count']} | {c['duplicates']} |")
    lines.append("")
    
    # Lost sync pulses
    lost = data["lost_sync_pulses"]
    lines.append("## Lost Sync Pulses (edge=0)")
    lines.append("")
    if lost:
        for device, seqs in sorted(lost.items()):
            lines.append(f"- **{device}**: {len(seqs)} lost, seqs={seqs}")
    else:
        lines.append("*(none)*")
    lines.append("")
    
    # Interpretation
    lines.append("## Interpretation")
    lines.append("")
    lines.append("**CRITICAL:** The two boards have **INDEPENDENT microsecond clocks** "
                 "(esp_timer_get_time() counts from each board's own boot; they are NOT synchronised). "
                 "Therefore we **NEVER** compute a difference between timestamps originating on different devices. "
                 "Every reported statistic is a **same-clock interval**:")
    lines.append("")
    lines.append("- TX-side (single device): `submit→pulse` and `pulse→sendcb`")
    lines.append("- RX-side (single device): `edge→rxcb` (**headline one-way latency**) and `rxcb→accept`")
    lines.append("")
    lines.append("The `edge→rxcb` interval captures radio TX + propagation + RX + callback delivery, "
                 "all measured on the receiver's own clock.")
    lines.append("")
    
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

def run_self_test() -> int:
    """Run the self-test with synthetic data.
    
    Proves:
    (a) A normal tx record and a normal rx record produce the four correct same-clock intervals
        with correct values.
    (b) A record with edge=0 lands in the "not measurable"/"lost pulse" bucket and does not
        corrupt other statistics.
    (c) A malformed/truncated line is skipped and counted, not crashed on.
    (d) A cross-device subtraction is NEVER performed — assert that the tool's computed
        intervals never mix devices (e.g. assert the tool can be handed two devices with
        wildly different clock bases, 1e6 µs apart, and still report identical, correct intervals).
    """
    import tempfile
    
    # Synthetic test lines
    # Device A: boot time 100000 µs
    # Device B: boot time 1100000 µs (1e6 µs apart to prove no cross-device subtraction)
    
    test_lines = [
        # Device boot timestamps
        "0.0 [DEVICE_A] I (100000) boot: starting\n",
        "0.0 [DEVICE_B] I (1100000) boot: starting\n",
        
        # (a) Normal TX record on DEVICE_A
        # submit=200000, pulse=200500, sendcb=201000 => submit->pulse=500, pulse->sendcb=500
        "10.0 [DEVICE_A] I (200000) task_session: BENCH_TIME seq=0 role=tx submit=200000 pulse=200500 sendcb=201000 edge=0 rxcb=0 accept=0\n",
        
        # (a) Normal RX record on DEVICE_B
        # edge=1200000, rxcb=1200800, accept=1201000 => edge->rxcb=800, rxcb->accept=200
        "10.1 [DEVICE_B] I (1200000) task_session: BENCH_TIME seq=0 role=rx submit=0 pulse=0 sendcb=0 edge=1200000 rxcb=1200800 accept=1201000\n",
        
        # (b) RX record with edge=0 (lost sync pulse) on DEVICE_A
        # Should land in lost_pulse_seqs and not_measurable, but rxcb->accept still measurable
        # rxcb=300500, accept=300700 => rxcb->accept=200
        "20.0 [DEVICE_A] I (300000) task_session: BENCH_TIME seq=5 role=rx submit=0 pulse=0 sendcb=0 edge=0 rxcb=300500 accept=300700\n",
        
        # (c) Malformed/truncated line - missing fields
        "30.0 [DEVICE_A] I (400000) task_session: BENCH_TIME seq=10 role=tx submit=400000 pulse=400500\n",
        
        # (c) Another malformed line - garbage
        "31.0 [DEVICE_B] W (1400000) some random garbage line\n",
        
        # (d) Another TX on DEVICE_B with very different clock base
        # submit=1500000, pulse=1500600, sendcb=1501200 => submit->pulse=600, pulse->sendcb=600
        "40.0 [DEVICE_B] I (1500000) task_session: BENCH_TIME seq=1 role=tx submit=1500000 pulse=1500600 sendcb=1501200 edge=0 rxcb=0 accept=0\n",
        
        # (d) Another RX on DEVICE_A with different clock base
        # edge=500000, rxcb=500900, accept=501100 => edge->rxcb=900, rxcb->accept=200
        "40.1 [DEVICE_A] I (500000) task_session: BENCH_TIME seq=1 role=rx submit=0 pulse=0 sendcb=0 edge=500000 rxcb=500900 accept=501100\n",
    ]
    
    with tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False, encoding="utf-8") as fh:
        fh.writelines(test_lines)
        temp_path = fh.name
    
    try:
        analyzer = LatencyAnalyzer()
        n = analyzer.process_file(temp_path)
        
        errors = []
        
        # Check parsing
        # 9 total lines: 2 boot + 5 valid BENCH_TIME + 2 malformed
        if analyzer.total_lines != 9:
            errors.append(f"total_lines: expected 9, got {analyzer.total_lines}")
        if len(analyzer.records) != 5:  # 5 valid BENCH_TIME records
            errors.append(f"records: expected 5, got {len(analyzer.records)}")
        if analyzer.skipped_lines != 4:  # 2 boot lines + 2 malformed
            errors.append(f"skipped_lines: expected 4, got {analyzer.skipped_lines}")
        
        # (a) Check TX intervals on DEVICE_A
        tx_sp = analyzer.tx_submit_pulse["DEVICE_A"].values_us
        tx_ps = analyzer.tx_pulse_sendcb["DEVICE_A"].values_us
        if tx_sp != [500]:
            errors.append(f"DEVICE_A tx submit->pulse: expected [500], got {tx_sp}")
        if tx_ps != [500]:
            errors.append(f"DEVICE_A tx pulse->sendcb: expected [500], got {tx_ps}")
        
        # (a) Check RX intervals on DEVICE_B
        rx_er = analyzer.rx_edge_rxcb["DEVICE_B"].values_us
        rx_ra = analyzer.rx_rxcb_accept["DEVICE_B"].values_us
        if rx_er != [800]:
            errors.append(f"DEVICE_B rx edge->rxcb: expected [800], got {rx_er}")
        if rx_ra != [200]:
            errors.append(f"DEVICE_B rx rxcb->accept: expected [200], got {rx_ra}")
        
        # (b) Check lost sync pulse handling
        if analyzer.lost_pulse_seqs["DEVICE_A"] != [5]:
            errors.append(f"lost_pulse_seqs DEVICE_A: expected [5], got {analyzer.lost_pulse_seqs['DEVICE_A']}")
        
        # (b) Check not_measurable for edge->rxcb on DEVICE_A (edge=0)
        nm_key = "DEVICE_A:rx:edge->rxcb"
        if analyzer.not_measurable.get(nm_key, 0) != 1:
            errors.append(f"not_measurable {nm_key}: expected 1, got {analyzer.not_measurable.get(nm_key, 0)}")
        
        # (b) But rxcb->accept should still be measurable on DEVICE_A
        # Note: there are 2 RX records on DEVICE_A (seq=5 edge=0, seq=1 edge=500000), both have rxcb->accept=200
        rx_ra_a = analyzer.rx_rxcb_accept["DEVICE_A"].values_us
        if sorted(rx_ra_a) != [200, 200]:
            errors.append(f"DEVICE_A rx rxcb->accept: expected [200, 200], got {sorted(rx_ra_a)}")
        
        # (d) Check TX intervals on DEVICE_B (different clock base)
        tx_sp_b = analyzer.tx_submit_pulse["DEVICE_B"].values_us
        tx_ps_b = analyzer.tx_pulse_sendcb["DEVICE_B"].values_us
        if tx_sp_b != [600]:
            errors.append(f"DEVICE_B tx submit->pulse: expected [600], got {tx_sp_b}")
        if tx_ps_b != [600]:
            errors.append(f"DEVICE_B tx pulse->sendcb: expected [600], got {tx_ps_b}")
        
        # (d) Check RX intervals on DEVICE_A (different clock base)
        rx_er_a = analyzer.rx_edge_rxcb["DEVICE_A"].values_us
        rx_ra_a2 = analyzer.rx_rxcb_accept["DEVICE_A"].values_us
        if rx_er_a != [900]:
            errors.append(f"DEVICE_A rx edge->rxcb: expected [900], got {rx_er_a}")
        # rx_ra_a2 should now have 2 values: [200, 200] from the two RX records
        if sorted(rx_ra_a2) != [200, 200]:
            errors.append(f"DEVICE_A rx rxcb->accept: expected [200, 200], got {sorted(rx_ra_a2)}")
        
        # (d) CRITICAL: Verify no cross-device intervals were computed
        # If we had mixed devices, the intervals would be wildly wrong (e.g. 1e6 µs off)
        # All intervals above are small (200-900 µs), proving same-clock computation
        all_intervals = (
            tx_sp + tx_ps + tx_sp_b + tx_ps_b +
            rx_er + rx_ra + rx_er_a + rx_ra_a2
        )
        # All intervals should be < 10000 µs (10 ms) - anything larger suggests cross-device mixing
        for iv in all_intervals:
            if iv > 10000:
                errors.append(f"interval {iv} µs exceeds 10ms threshold - possible cross-device subtraction!")
        
        # Verify the data dict exports correctly
        data = analyzer.to_dict("test_run")
        if data["clock_used"] != "device":
            errors.append(f"clock_used: expected 'device', got {data['clock_used']}")
        if "interpretation" not in data:
            errors.append("interpretation block missing from data dict")
        
        if errors:
            print("[FAIL] Self-test failed:")
            for err in errors:
                print(f"  - {err}")
            return 1
        
        print("[PASS] Self-test passed")
        print("  - Normal TX/RX records produce correct same-clock intervals")
        print("  - edge=0 records land in lost_pulse/not_measurable buckets")
        print("  - Malformed lines are skipped and counted")
        print("  - No cross-device subtraction (all intervals < 10ms despite 1e6 µs clock offset)")
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
    analyzer = LatencyAnalyzer()
    n_lines = analyzer.process_file(log_path)
    if n_lines < 0:
        return 1
    
    if args.verbose:
        print(f"Processed {n_lines} lines from {log_path}")
    
    # Generate data dict
    data = analyzer.to_dict(run_id)
    
    # Check if we have any BENCH_TIME records
    if len(analyzer.records) == 0:
        if not args.quiet:
            print("[WARN] No BENCH_TIME records found in log file", file=sys.stderr)
            print("The log may be from a different experiment or the firmware may not emit these records yet.")
        # Still write outputs with empty data
    
    # Generate outputs
    if not args.quiet:
        report = generate_report(data)
        print(report)
    
    # Determine output paths
    derived_dir = os.path.join(args.derived_dir, run_id) if args.run_id else args.derived_dir
    os.makedirs(derived_dir, exist_ok=True)
    
    json_path = args.json if args.json else os.path.join(derived_dir, "latency.json")
    md_path = args.md if args.md else os.path.join(derived_dir, "latency.md")
    
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
#!/usr/bin/env python3
"""tools/e1_frame_lengths.py — frame length extraction from CEE-PEW E1 capture logs.

Parses the human-readable instrumentation lines from E1 capture experiments to
extract frame length statistics. This is the PRIMARY evidence source when ring
dump lines are corrupted by UART interleaving.

Patterns extracted:
  1. ARQ TX:      [ARQ_TX] len=N seq=0xXX
  2. ARQ RX:      [ARQ_RX] ... (no len field in current firmware)
  3. RADIO RX:    RADIO-RX: frame from MAC len=N
  4. SECURE_CHAT: [SECURE_CHAT_TX] Initiating TX: len=N bytes
  5. BENCH58 TX:  [BENCH58_TX] seq=N len=N (including forged variants)
  6. BENCH58 DONE:[BENCH58_DONE] sent=N

Ring-line truncation audit:
  Scans lines matching the ring-dump format ts,rssi,ch,len,HEX and classifies
  each as CLEAN or CORRUPTED to quantify the length-bias in corruption.

Usage:
  python tools/e1_frame_lengths.py --run-id E1_A_plain_20261018
  python tools/e1_frame_lengths.py --log path/to/capture_dual.log
  python tools/e1_frame_lengths.py --self-test
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict
from typing import Dict, List, Optional, Tuple, Any


# --------------------------------------------------------------------------
# Regex patterns for extraction
# --------------------------------------------------------------------------

# Device label extraction: [DEVICE_A] or [DEVICE_B] or [COM3] etc before I (/W (/E (
DEVICE_LABEL_RE = re.compile(r"\[([^\]]+)\]\s*(?:[IWEVD]\s*\(\s*\d+\s*\))")

# ARQ TX: [ARQ_TX] len=N seq=0xXX (may have garbage prefix from interleaving)
ARQ_TX_RE = re.compile(r"\[ARQ_TX\]\s+len=(\d+)\s+seq=0x([0-9A-Fa-f]+)")

# ARQ RX: [ARQ_RX] ... (no len field in current firmware; we count occurrences)
ARQ_RX_RE = re.compile(r"\[ARQ_RX\]")

# RADIO RX: RADIO-RX: frame from MAC len=N
RADIO_RX_RE = re.compile(r"RADIO-RX:\s+frame from ([0-9A-Fa-f:]+)\s+len=(\d+)")

# SECURE CHAT TX: [SECURE_CHAT_TX] Initiating TX: len=N bytes, peer_mac=MAC
SECURE_CHAT_TX_RE = re.compile(
    r"\[SECURE_CHAT_TX\]\s+Initiating TX:\s+len=(\d+)\s+bytes,\s+peer_mac=([0-9A-Fa-f:]+)"
)

# BENCH58 TX: [BENCH58_TX] seq=N len=N (may also have "forged" suffix)
BENCH58_TX_RE = re.compile(r"\[BENCH58_TX\]\s+seq=(\d+)\s+len=(\d+)")
BENCH58_FORGED_RE = re.compile(r"\[BENCH58_TX\]\s+seq=(\d+)\s+forged")

# BENCH58 DONE: [BENCH58_DONE] sent=N
BENCH58_DONE_RE = re.compile(r"\[BENCH58_DONE\]\s+sent=(\d+)")

# Ring dump line: ts,rssi,ch,len,HEX...
# A row is CLEAN iff: len(hex)%2==0 AND len(hex)//2 == declared_len AND rest of line is blank
RING_LINE_RE = re.compile(r"^(\d{4,}),(-?\d{1,4}),(\d{1,2}),(\d{1,4}),([0-9A-Fa-f]*)")


# --------------------------------------------------------------------------
# Line parsing
# --------------------------------------------------------------------------

def extract_device_label(line: str) -> str:
    """Extract device label from a log line.
    
    Device label = the [...] token immediately before the ESP-IDF I (/W (/E ( marker.
    Fall back to [COMx] if present, else UNKNOWN.
    """
    # Try the primary pattern: [LABEL] I ( or [LABEL] W ( etc
    m = DEVICE_LABEL_RE.search(line)
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


def classify_ring_line(line: str) -> Tuple[bool, Optional[int], str]:
    """Classify a ring dump line as CLEAN or CORRUPTED.
    
    Returns: (is_clean, declared_len, hex_data)
    - is_clean: True if the ring row is intact
    - declared_len: The length field from the ring line (or None if not matched)
    - hex_data: The hex portion (or empty string)
    
    A row is CLEAN iff:
    1. It matches the ring pattern
    2. len(hex) % 2 == 0
    3. len(hex) // 2 == declared_len
    4. The remainder of the line after the hex is blank (no interleaved content)
    """
    # Find ring pattern anywhere in line (it may have prefix garbage)
    m = re.search(r"(\d{4,}),(-?\d{1,4}),(\d{1,2}),(\d{1,4}),([0-9A-Fa-f]*)", line)
    if not m:
        return (False, None, "")
    
    declared_len = int(m.group(4))
    hex_data = m.group(5)
    
    # Find where the hex ends in the original line
    hex_end_pos = m.end()
    remainder = line[hex_end_pos:].strip()
    
    # Check cleanliness criteria
    if len(hex_data) % 2 != 0:
        return (False, declared_len, hex_data)
    
    actual_len = len(hex_data) // 2
    if actual_len != declared_len:
        return (False, declared_len, hex_data)
    
    # Check if remainder is blank (allowing for whitespace only)
    if remainder and not re.match(r"^\s*$", remainder):
        # There's interleaved content after the hex
        return (False, declared_len, hex_data)
    
    return (True, declared_len, hex_data)


# --------------------------------------------------------------------------
# Analysis
# --------------------------------------------------------------------------

class FrameLengthAnalyzer:
    def __init__(self):
        # Per-source counts
        self.arq_tx_count = 0
        self.arq_rx_count = 0
        self.radio_rx_count = 0
        self.secure_chat_tx_count = 0
        self.bench58_tx_count = 0
        self.bench58_tx_forged_count = 0
        self.bench58_done_count = 0
        self.bench58_done_sent_total = 0
        
        # Length histograms (pooled)
        self.arq_tx_lengths: Counter = Counter()
        self.radio_rx_lengths: Counter = Counter()
        self.secure_chat_tx_lengths: Counter = Counter()
        self.bench58_tx_lengths: Counter = Counter()
        
        # Per-device histograms
        self.arq_tx_lengths_by_device: Dict[str, Counter] = defaultdict(Counter)
        self.radio_rx_lengths_by_device: Dict[str, Counter] = defaultdict(Counter)
        self.secure_chat_tx_lengths_by_device: Dict[str, Counter] = defaultdict(Counter)
        self.bench58_tx_lengths_by_device: Dict[str, Counter] = defaultdict(Counter)
        
        # ARQ sequence tracking
        self.arq_tx_seqs: List[int] = []
        self.arq_tx_seqs_by_device: Dict[str, List[int]] = defaultdict(list)
        
        # Ring line audit
        self.ring_total = 0
        self.ring_clean = 0
        self.ring_corrupted = 0
        self.ring_clean_lengths: Counter = Counter()
        self.ring_corrupted_lengths: Counter = Counter()
        
        # Per-device counts
        self.device_counts: Counter = Counter()
    
    def process_line(self, line: str) -> None:
        """Process a single log line."""
        device = extract_device_label(line)
        self.device_counts[device] += 1
        
        # Track whether we matched any structured pattern
        matched_structured = False
        
        # ARQ TX
        m = ARQ_TX_RE.search(line)
        if m:
            length = int(m.group(1))
            seq = int(m.group(2), 16)
            self.arq_tx_count += 1
            self.arq_tx_lengths[length] += 1
            self.arq_tx_lengths_by_device[device][length] += 1
            self.arq_tx_seqs.append(seq)
            self.arq_tx_seqs_by_device[device].append(seq)
            matched_structured = True
        
        # ARQ RX (no length field)
        if ARQ_RX_RE.search(line):
            self.arq_rx_count += 1
            matched_structured = True
        
        # RADIO RX
        m = RADIO_RX_RE.search(line)
        if m:
            length = int(m.group(2))
            self.radio_rx_count += 1
            self.radio_rx_lengths[length] += 1
            self.radio_rx_lengths_by_device[device][length] += 1
            matched_structured = True
        
        # SECURE CHAT TX
        m = SECURE_CHAT_TX_RE.search(line)
        if m:
            length = int(m.group(1))
            self.secure_chat_tx_count += 1
            self.secure_chat_tx_lengths[length] += 1
            self.secure_chat_tx_lengths_by_device[device][length] += 1
            matched_structured = True
        
        # BENCH58 TX (check forged first to count separately)
        m_forged = BENCH58_FORGED_RE.search(line)
        if m_forged:
            self.bench58_tx_forged_count += 1
            matched_structured = True
            # Still count as a TX if it also has len
            m = BENCH58_TX_RE.search(line)
            if m:
                length = int(m.group(2))
                self.bench58_tx_count += 1
                self.bench58_tx_lengths[length] += 1
                self.bench58_tx_lengths_by_device[device][length] += 1
        
        if not m_forged:
            m = BENCH58_TX_RE.search(line)
            if m:
                length = int(m.group(2))
                self.bench58_tx_count += 1
                self.bench58_tx_lengths[length] += 1
                self.bench58_tx_lengths_by_device[device][length] += 1
                matched_structured = True
        
        # BENCH58 DONE
        m = BENCH58_DONE_RE.search(line)
        if m:
            self.bench58_done_count += 1
            self.bench58_done_sent_total += int(m.group(1))
            matched_structured = True
        
        # Ring line audit: ALWAYS run, even if we matched other patterns
        # This quantifies UART interleaving corruption independently
        is_clean, declared_len, hex_data = classify_ring_line(line)
        if declared_len is not None:
            self.ring_total += 1
            if is_clean:
                self.ring_clean += 1
                self.ring_clean_lengths[declared_len] += 1
            else:
                self.ring_corrupted += 1
                self.ring_corrupted_lengths[declared_len] += 1
    
    def process_file(self, path: str) -> int:
        """Process an entire log file. Returns number of lines processed."""
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    self.process_line(line)
        except OSError as exc:
            print(f"[FAIL] {path}: {exc}", file=sys.stderr)
            return -1
        return sum(self.device_counts.values())
    
    def arq_seq_coverage(self, seqs: Optional[List[int]] = None) -> Dict[str, Any]:
        """Compute ARQ TX sequence coverage metrics."""
        if seqs is None:
            seqs = self.arq_tx_seqs
        
        if not seqs:
            return {
                "min": None,
                "max": None,
                "unique": 0,
                "gaps": [],
                "gap_count": 0,
            }
        
        unique_seqs = sorted(set(seqs))
        min_seq = min(unique_seqs)
        max_seq = max(unique_seqs)
        expected = set(range(min_seq, max_seq + 1))
        gaps = sorted(expected - set(unique_seqs))
        
        return {
            "min": min_seq,
            "max": max_seq,
            "unique": len(unique_seqs),
            "gaps": gaps,
            "gap_count": len(gaps),
        }
    
    def to_dict(self, run_id: str) -> Dict[str, Any]:
        """Export results as a dictionary for JSON serialization."""
        arq_coverage = self.arq_seq_coverage()
        
        return {
            "run_id": run_id,
            "source_counts": {
                "arq_tx": self.arq_tx_count,
                "arq_rx": self.arq_rx_count,
                "radio_rx": self.radio_rx_count,
                "secure_chat_tx": self.secure_chat_tx_count,
                "bench58_tx": self.bench58_tx_count,
                "bench58_tx_forged": self.bench58_tx_forged_count,
                "bench58_done": self.bench58_done_count,
                "bench58_done_sent_total": self.bench58_done_sent_total,
            },
            "histograms": {
                "arq_tx": dict(self.arq_tx_lengths),
                "radio_rx": dict(self.radio_rx_lengths),
                "secure_chat_tx": dict(self.secure_chat_tx_lengths),
                "bench58_tx": dict(self.bench58_tx_lengths),
            },
            "histograms_by_device": {
                "arq_tx": {k: dict(v) for k, v in self.arq_tx_lengths_by_device.items()},
                "radio_rx": {k: dict(v) for k, v in self.radio_rx_lengths_by_device.items()},
                "secure_chat_tx": {k: dict(v) for k, v in self.secure_chat_tx_lengths_by_device.items()},
                "bench58_tx": {k: dict(v) for k, v in self.bench58_tx_lengths_by_device.items()},
            },
            "arq_seq_coverage": arq_coverage,
            "ring_truncation_audit": {
                "total": self.ring_total,
                "clean": self.ring_clean,
                "corrupted": self.ring_corrupted,
                "clean_pct": round(100.0 * self.ring_clean / self.ring_total, 1) if self.ring_total > 0 else 0.0,
                "clean_length_histogram": dict(self.ring_clean_lengths),
                "corrupted_length_histogram": dict(self.ring_corrupted_lengths),
            },
            "limitations": (
                "Ring/pcap-derived length histograms from this run are length-biased due to UART "
                "interleaving corruption and must not be used as the primary frame-size evidence. "
                "The intact short-line histograms above are the primary source."
            ),
        }


# --------------------------------------------------------------------------
# Report formatting
# --------------------------------------------------------------------------

def fmt_histogram(hist: Counter, max_items: int = 10) -> str:
    """Format a length histogram compactly."""
    if not hist:
        return "(none)"
    items = sorted(hist.items())
    if len(items) > max_items:
        # Show min, max, and most common
        top = hist.most_common(max_items - 2)
        top.sort()
        min_k, min_v = items[0]
        max_k, max_v = items[-1]
        parts = [f"{min_k}:{min_v}"]
        for k, v in top:
            if k != min_k and k != max_k:
                parts.append(f"{k}:{v}")
        parts.append(f"{max_k}:{max_v}")
        return "{" + ", ".join(str(p) for p in parts) + "}"
    return "{" + ", ".join(f"{k}:{v}" for k, v in items) + "}"


def generate_report(analyzer: FrameLengthAnalyzer, run_id: str) -> str:
    """Generate the human-readable report."""
    lines = []
    lines.append("=" * 96)
    lines.append("CEE-PEW E1 FRAME LENGTH REPORT")
    lines.append("=" * 96)
    lines.append(f"run_id: {run_id}")
    lines.append("")
    
    # Source counts
    lines.append("-- source counts --")
    lines.append(f"  ARQ_TX            : {analyzer.arq_tx_count}")
    lines.append(f"  ARQ_RX            : {analyzer.arq_rx_count}")
    lines.append(f"  RADIO_RX          : {analyzer.radio_rx_count}")
    lines.append(f"  SECURE_CHAT_TX    : {analyzer.secure_chat_tx_count}")
    lines.append(f"  BENCH58_TX        : {analyzer.bench58_tx_count} (forged: {analyzer.bench58_tx_forged_count})")
    lines.append(f"  BENCH58_DONE      : {analyzer.bench58_done_count} (sent_total: {analyzer.bench58_done_sent_total})")
    lines.append("")
    
    # Length histograms
    lines.append("-- length histograms (pooled) --")
    lines.append(f"  ARQ_TX         : {fmt_histogram(analyzer.arq_tx_lengths)}")
    lines.append(f"  RADIO_RX       : {fmt_histogram(analyzer.radio_rx_lengths)}")
    lines.append(f"  SECURE_CHAT_TX : {fmt_histogram(analyzer.secure_chat_tx_lengths)}")
    lines.append(f"  BENCH58_TX     : {fmt_histogram(analyzer.bench58_tx_lengths)}")
    lines.append("")
    
    # Per-device breakdown
    lines.append("-- per-device breakdown --")
    for device in sorted(analyzer.device_counts.keys()):
        if device == "UNKNOWN":
            continue
        lines.append(f"  [{device}]:")
        if analyzer.arq_tx_lengths_by_device.get(device):
            lines.append(f"    ARQ_TX         : {fmt_histogram(analyzer.arq_tx_lengths_by_device[device])}")
        if analyzer.radio_rx_lengths_by_device.get(device):
            lines.append(f"    RADIO_RX       : {fmt_histogram(analyzer.radio_rx_lengths_by_device[device])}")
        if analyzer.secure_chat_tx_lengths_by_device.get(device):
            lines.append(f"    SECURE_CHAT_TX : {fmt_histogram(analyzer.secure_chat_tx_lengths_by_device[device])}")
        if analyzer.bench58_tx_lengths_by_device.get(device):
            lines.append(f"    BENCH58_TX     : {fmt_histogram(analyzer.bench58_tx_lengths_by_device[device])}")
    lines.append("")
    
    # ARQ sequence coverage
    coverage = analyzer.arq_seq_coverage()
    lines.append("-- ARQ TX sequence coverage --")
    if coverage["min"] is not None:
        lines.append(f"  seq range: 0x{coverage['min']:02X}..0x{coverage['max']:02X}")
        lines.append(f"  unique: {coverage['unique']}, gaps: {coverage['gap_count']}")
        if coverage["gaps"]:
            gap_str = ", ".join(f"0x{g:02X}" for g in coverage["gaps"][:20])
            if len(coverage["gaps"]) > 20:
                gap_str += f" ... ({len(coverage['gaps']) - 20} more)"
            lines.append(f"  missing: {gap_str}")
        else:
            lines.append("  => ZERO gaps in sequence (complete coverage)")
    else:
        lines.append("  (no ARQ_TX sequences found)")
    lines.append("")
    
    # Ring truncation audit
    lines.append("-- ring-line truncation audit --")
    lines.append(f"  total ring rows  : {analyzer.ring_total}")
    lines.append(f"  clean            : {analyzer.ring_clean} ({100.0 * analyzer.ring_clean / analyzer.ring_total:.1f}%)" if analyzer.ring_total > 0 else "  clean            : 0 (0.0%)")
    lines.append(f"  corrupted        : {analyzer.ring_corrupted} ({100.0 * analyzer.ring_corrupted / analyzer.ring_total:.1f}%)" if analyzer.ring_total > 0 else "  corrupted        : 0 (0.0%)")
    if analyzer.ring_clean_lengths:
        lines.append(f"  clean lengths    : {fmt_histogram(analyzer.ring_clean_lengths)}")
    if analyzer.ring_corrupted_lengths:
        lines.append(f"  corrupted lengths: {fmt_histogram(analyzer.ring_corrupted_lengths)}")
    lines.append("")
    
    # Interpretation
    lines.append("-- INTERPRETATION --")
    if analyzer.ring_total > 0:
        clean_pct = 100.0 * analyzer.ring_clean / analyzer.ring_total
        lines.append(f"  Ring/pcap-derived length histograms from this run are LENGTH-BIASED")
        lines.append(f"  (only {clean_pct:.1f}% of ring rows are clean; longer frames are preferentially corrupted).")
        lines.append(f"  DO NOT use pcap-derived histograms as primary frame-size evidence.")
        lines.append(f"  The intact short-line histograms above are the PRIMARY source.")
    else:
        lines.append("  No ring dump lines found; short-line histograms are the only source.")
    lines.append(f"  Sample sizes: ARQ_TX n={analyzer.arq_tx_count}, RADIO_RX n={analyzer.radio_rx_count},")
    lines.append(f"                SECURE_CHAT_TX n={analyzer.secure_chat_tx_count}, BENCH58_TX n={analyzer.bench58_tx_count}")
    lines.append("")
    
    return "\n".join(lines)


def generate_markdown_report(analyzer: FrameLengthAnalyzer, run_id: str) -> str:
    """Generate the Markdown report for the paper."""
    lines = []
    lines.append(f"# E1 Frame Length Report: {run_id}")
    lines.append("")
    lines.append("## Source Counts")
    lines.append("")
    lines.append("| Source | Count |")
    lines.append("|--------|-------|")
    lines.append(f"| ARQ_TX | {analyzer.arq_tx_count} |")
    lines.append(f"| ARQ_RX | {analyzer.arq_rx_count} |")
    lines.append(f"| RADIO_RX | {analyzer.radio_rx_count} |")
    lines.append(f"| SECURE_CHAT_TX | {analyzer.secure_chat_tx_count} |")
    lines.append(f"| BENCH58_TX | {analyzer.bench58_tx_count} (forged: {analyzer.bench58_tx_forged_count}) |")
    lines.append(f"| BENCH58_DONE | {analyzer.bench58_done_count} (sent: {analyzer.bench58_done_sent_total}) |")
    lines.append("")
    
    lines.append("## Length Histograms")
    lines.append("")
    lines.append("### ARQ TX")
    lines.append(f"```")
    lines.append(fmt_histogram(analyzer.arq_tx_lengths))
    lines.append("```")
    lines.append("")
    
    lines.append("### RADIO RX")
    lines.append(f"```")
    lines.append(fmt_histogram(analyzer.radio_rx_lengths))
    lines.append("```")
    lines.append("")
    
    lines.append("### SECURE CHAT TX")
    lines.append(f"```")
    lines.append(fmt_histogram(analyzer.secure_chat_tx_lengths))
    lines.append("```")
    lines.append("")
    
    lines.append("### BENCH58 TX")
    lines.append(f"```")
    lines.append(fmt_histogram(analyzer.bench58_tx_lengths))
    lines.append("```")
    lines.append("")
    
    coverage = analyzer.arq_seq_coverage()
    lines.append("## ARQ Sequence Coverage")
    lines.append("")
    if coverage["min"] is not None:
        lines.append(f"- Range: 0x{coverage['min']:02X}..0x{coverage['max']:02X}")
        lines.append(f"- Unique: {coverage['unique']}")
        lines.append(f"- Gaps: {coverage['gap_count']}")
        if coverage["gaps"]:
            lines.append(f"- Missing: {coverage['gaps']}")
        else:
            lines.append("- **ZERO gaps** (complete coverage)")
    lines.append("")
    
    lines.append("## Ring-Line Truncation Audit")
    lines.append("")
    lines.append(f"- Total ring rows: {analyzer.ring_total}")
    if analyzer.ring_total > 0:
        clean_pct = 100.0 * analyzer.ring_clean / analyzer.ring_total
        lines.append(f"- Clean: {analyzer.ring_clean} ({clean_pct:.1f}%)")
        lines.append(f"- Corrupted: {analyzer.ring_corrupted} ({100.0 - clean_pct:.1f}%)")
        if analyzer.ring_clean_lengths:
            lines.append(f"- Clean length histogram: `{fmt_histogram(analyzer.ring_clean_lengths)}`")
        if analyzer.ring_corrupted_lengths:
            lines.append(f"- Corrupted length histogram: `{fmt_histogram(analyzer.ring_corrupted_lengths)}`")
    lines.append("")
    
    lines.append("## Limitations")
    lines.append("")
    lines.append(analyzer.to_dict(run_id)["limitations"])
    lines.append("")
    
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

def run_self_test() -> int:
    """Run the self-test with synthetic data."""
    import tempfile
    
    # Synthetic test lines
    # Clean ring row: declared_len=10, hex=20 chars (10 bytes), no remainder
    clean_hex = "0102030405060708090A"
    test_lines = [
        # Clean ring row
        f"12345,-65,1,10,{clean_hex}\n",
        # Truncated ring row (hex length doesn't match declared: 50 vs 8 bytes)
        "12346,-65,1,50,0102030405060708\n",
        # Interleaved ring row (ring hex followed by ESP log) - declared=10, hex=20 chars, but has remainder
        f"12347,-65,1,10,{clean_hex}I (123) foo: [ARQ_TX] len=78 seq=0x0D\n",
        # Normal ARQ_TX (standalone, not from interleaved ring)
        "59.2 [DEVICE_A] I (44732) ARQ: [ARQ_TX] len=77 seq=0x00\n",
        # BENCH58_TX forged
        "105.6 [DEVICE_B] I (105386) task_session: [BENCH58_TX] seq=9 forged (tag byte will be corrupted)\n",
        # BENCH58_TX normal
        "59.9 [DEVICE_B] I (59713) task_session: [BENCH58_TX] seq=0 len=58\n",
        # RADIO_RX
        "58.7 [DEVICE_B] I (58508) RADIO-RX: frame from 1C:C3:AB:C2:52:AC len=2 data[0]=0xbb\n",
        # SECURE_CHAT_TX
        "59.4 [DEVICE_A] I (44871) session_send: [SECURE_CHAT_TX] Initiating TX: len=1 bytes, peer_mac=8C:94:DF:97:D0:C0\n",
        # ARQ_RX
        "59.3 [DEVICE_A] I (44755) ARQ: [ARQ_RX] first-frame accept seq=0xBB -> expected=188\n",
        # BENCH58_DONE
        "460.3 [DEVICE_B] I (460102) task_session: [BENCH58_DONE] sent=80\n",
    ]
    
    with tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False, encoding="utf-8") as fh:
        fh.writelines(test_lines)
        temp_path = fh.name
    
    try:
        analyzer = FrameLengthAnalyzer()
        n = analyzer.process_file(temp_path)
        
        errors = []
        
        # Check ring audit
        if analyzer.ring_total != 3:
            errors.append(f"ring_total: expected 3, got {analyzer.ring_total}")
        if analyzer.ring_clean != 1:
            errors.append(f"ring_clean: expected 1, got {analyzer.ring_clean}")
        if analyzer.ring_corrupted != 2:
            errors.append(f"ring_corrupted: expected 2, got {analyzer.ring_corrupted}")
        
        # Check ARQ_TX (note: interleaved line also contains ARQ_TX pattern, so count=2)
        if analyzer.arq_tx_count != 2:
            errors.append(f"arq_tx_count: expected 2, got {analyzer.arq_tx_count}")
        if analyzer.arq_tx_lengths != Counter({77: 1, 78: 1}):
            errors.append(f"arq_tx_lengths: expected {{77: 1, 78: 1}}, got {dict(analyzer.arq_tx_lengths)}")
        
        # Check ARQ_RX
        if analyzer.arq_rx_count != 1:
            errors.append(f"arq_rx_count: expected 1, got {analyzer.arq_rx_count}")
        
        # Check RADIO_RX
        if analyzer.radio_rx_count != 1:
            errors.append(f"radio_rx_count: expected 1, got {analyzer.radio_rx_count}")
        if analyzer.radio_rx_lengths != Counter({2: 1}):
            errors.append(f"radio_rx_lengths: expected {{2: 1}}, got {dict(analyzer.radio_rx_lengths)}")
        
        # Check SECURE_CHAT_TX
        if analyzer.secure_chat_tx_count != 1:
            errors.append(f"secure_chat_tx_count: expected 1, got {analyzer.secure_chat_tx_count}")
        if analyzer.secure_chat_tx_lengths != Counter({1: 1}):
            errors.append(f"secure_chat_tx_lengths: expected {{1: 1}}, got {dict(analyzer.secure_chat_tx_lengths)}")
        
        # Check BENCH58_TX
        if analyzer.bench58_tx_count != 1:
            errors.append(f"bench58_tx_count: expected 1, got {analyzer.bench58_tx_count}")
        if analyzer.bench58_tx_forged_count != 1:
            errors.append(f"bench58_tx_forged_count: expected 1, got {analyzer.bench58_tx_forged_count}")
        if analyzer.bench58_tx_lengths != Counter({58: 1}):
            errors.append(f"bench58_tx_lengths: expected {{58: 1}}, got {dict(analyzer.bench58_tx_lengths)}")
        
        # Check BENCH58_DONE
        if analyzer.bench58_done_count != 1:
            errors.append(f"bench58_done_count: expected 1, got {analyzer.bench58_done_count}")
        if analyzer.bench58_done_sent_total != 80:
            errors.append(f"bench58_done_sent_total: expected 80, got {analyzer.bench58_done_sent_total}")
        
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
    analyzer = FrameLengthAnalyzer()
    n_lines = analyzer.process_file(log_path)
    if n_lines < 0:
        return 1
    
    if args.verbose:
        print(f"Processed {n_lines} lines from {log_path}")
    
    # Generate outputs
    if not args.quiet:
        report = generate_report(analyzer, run_id)
        print(report)
    
    # Determine output paths
    derived_dir = os.path.join(args.derived_dir, run_id) if args.run_id else args.derived_dir
    os.makedirs(derived_dir, exist_ok=True)
    
    json_path = args.json if args.json else os.path.join(derived_dir, "frame_lengths.json")
    md_path = args.md if args.md else os.path.join(derived_dir, "frame_lengths.md")
    
    # Write JSON
    data = analyzer.to_dict(run_id)
    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)
        fh.write("\n")
    
    # Write Markdown
    md_report = generate_markdown_report(analyzer, run_id)
    with open(md_path, "w", encoding="utf-8") as fh:
        fh.write(md_report)
        fh.write("\n")
    
    if not args.quiet:
        print(f"wrote {json_path}")
        print(f"wrote {md_path}")
    
    return 0


if __name__ == "__main__":
    sys.exit(main())

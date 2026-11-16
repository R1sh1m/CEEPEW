#!/usr/bin/env python3
"""
tools/extract_frame.py — Extract frame hex from CEE-PEW capture logs.

Parses dual-port capture logs and extracts the hex payload of transmitted
frames for use with sniffer_inject.py (OTA replay testing).

Looks for lines matching:
  [SECURE_CHAT_TX] ... len=N ... : <hex_payload>

Usage:
  python tools/extract_frame.py --log logs/A1_pair.log --out logs/A1_frame.hex
  python tools/extract_frame.py --log logs/A1_pair.log --out logs/A1_frame.hex --seq 5
"""

from __future__ import annotations

import argparse
import re
import sys
from typing import Optional, List


# Match lines like: [SECURE_CHAT_TX] Initiating TX: len=58 bytes, peer_mac=...
# Followed by a hex dump line or inline hex
RE_TX_INIT = re.compile(
    r"\[SECURE_CHAT_TX\]\s+Initiating TX:\s*len=(\d+)\s*bytes"
)
RE_HEX_LINE = re.compile(r"\b([0-9a-fA-F]{8,})\b")


def extract_frames(log_path: str, seq_filter: Optional[int] = None,
                   max_frames: Optional[int] = None) -> List[str]:
    """Extract hex frame payloads from a capture log.

    Args:
        log_path: Path to the capture log file.
        seq_filter: If set, only extract frames matching this sequence number.
        max_frames: Maximum number of frames to extract.

    Returns:
        List of hex strings (one per frame).
    """
    frames: List[str] = []
    current_len: Optional[int] = None

    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            # Look for TX initiation line
            m = RE_TX_INIT.search(line)
            if m:
                current_len = int(m.group(1))
                continue

            # Look for hex payload on subsequent lines
            if current_len is not None:
                hm = RE_HEX_LINE.search(line)
                if hm:
                    hex_str = hm.group(1)
                    # Basic sanity: hex length should match 2 * byte count
                    if len(hex_str) >= current_len * 2:
                        frames.append(hex_str[: current_len * 2])
                        current_len = None
                        if max_frames and len(frames) >= max_frames:
                            break

    return frames


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Extract frame hex payloads from CEE-PEW capture logs."
    )
    parser.add_argument("--log", required=True, help="Capture log file")
    parser.add_argument("--out", required=True, help="Output hex file (one frame per line)")
    parser.add_argument("--seq", type=int, default=None,
                        help="Only extract frames matching this sequence number")
    parser.add_argument("--max", type=int, default=None,
                        help="Maximum number of frames to extract")
    args = parser.parse_args(argv)

    frames = extract_frames(args.log, seq_filter=args.seq, max_frames=args.max)

    if not frames:
        print("[WARNING] No frames extracted from log.", file=sys.stderr)
        return 1

    with open(args.out, "w") as f:
        for hex_str in frames:
            f.write(hex_str + "\n")

    print(f"Extracted {len(frames)} frame(s) to {args.out}")
    for i, hex_str in enumerate(frames):
        print(f"  Frame {i}: {len(hex_str)//2} bytes, hex={hex_str[:32]}...")

    return 0


if __name__ == "__main__":
    sys.exit(main())

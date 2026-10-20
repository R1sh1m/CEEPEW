#!/usr/bin/env python3
"""tools/promisc_capture_helper.py — 3-mode RF capture helper (host-only skeleton).

Produces court-admissible promiscuous capture logs for the CEE-PEW RF lane:
  mode A — plain ESP-NOW (no encryption)
  mode B — ESP-NOW + Core CCMP (baseline encrypted transport)
  mode C — CEE-PEW full stack (Ascon-128 AEAD session)

Fixed test vector (all modes): CMD=OPEN_VALVE_04;AUTH=ADMIN
Target: ~30 frames per mode, channel hopping OFF (fixed channel, default 1).

No firmware edits. This helper runs on the host: it drives the two DUTs over
serial (via tools/ceepew_monitor.py conventions), parses the on-node
promiscuous ring-dump lines (``ts,rssi,ch,len,hex``), and runs the offline
entropy + replay-of-plaintext checks.

Companion docs:
  tools/capture/capture_session.md  — on-node promiscuous + serial ring-dump path
  tools/capture/ota_sniffer_plan.md — 3rd-ESP32 sniffer firmware plan
  tools/capture/serial_to_pcap.py   — log -> pcap converter
  tools/capture/analyze_pcap.py     — pcap analysis

Usage:
  python tools/promisc_capture_helper.py --mode plain --port COM5 --frames 30 --channel 1
  python tools/promisc_capture_helper.py --mode ccmp --port COM5 --frames 30 --channel 1
  python tools/promisc_capture_helper.py --mode ceepew --port COM5 --frames 30 --channel 1
  python tools/promisc_capture_helper.py --analyze capture_run1.txt
  python tools/promisc_capture_helper.py --self-test

Capture procedure (bench):
  1. Fix DUTs + sniffer to one channel (default 1). Hopping OFF for the run.
  2. Send the fixed vector CMD=OPEN_VALVE_04;AUTH=ADMIN ~30 times per mode.
  3. Log with: python tools/ceepew_monitor.py --ports COM5,COM6 --baud 921600
     --duration 900 --log capture_runN.txt   (see capture_session.md section 1)
  4. Convert: python tools/capture/serial_to_pcap.py capture_runN.txt
     -o runN_ch1_<mode>.pcap --radiotap
  5. Analyze: python tools/promisc_capture_helper.py --analyze capture_runN.txt
"""

from __future__ import annotations

import argparse
import math
import os
import re
import sys
from collections import Counter

FIXED_MESSAGE = "CMD=OPEN_VALVE_04;AUTH=ADMIN"
DEFAULT_FRAMES_PER_MODE = 30
DEFAULT_CHANNEL = 1
MODES = ("plain", "ccmp", "ceepew")

# On-node promiscuous ring-dump line: ts,rssi,ch,len,hex
RING_LINE_RE = re.compile(
    r"^\s*(?P<ts>\d+)\s*,\s*(?P<rssi>-?\d+)\s*,\s*(?P<ch>\d+)\s*,"
    r"\s*(?P<length>\d+)\s*,\s*(?P<hex>[0-9a-fA-F]+)\s*$"
)


def parse_ring_log(path: str) -> list[dict]:
    """Parse ring-dump lines; skip boot/diagnostic text. Returns frame dicts."""
    frames: list[dict] = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line_no, raw in enumerate(handle, start=1):
            match = RING_LINE_RE.match(raw.strip())
            if not match:
                continue
            payload = bytes.fromhex(match.group("hex"))
            frames.append(
                {
                    "line_no": line_no,
                    "ts": int(match.group("ts")),
                    "rssi": int(match.group("rssi")),
                    "ch": int(match.group("ch")),
                    "length": int(match.group("length")),
                    "payload": payload,
                }
            )
    return frames


def shannon_entropy(data: bytes) -> float:
    """Shannon entropy in bits per byte."""
    if not data:
        return 0.0
    counts = Counter(data)
    total = len(data)
    return -sum((count / total) * math.log2(count / total) for count in counts.values())


def check_replay_of_plaintext(frames: list[dict]) -> dict:
    """Flag any frame whose payload carries the fixed vector in cleartext.

    Expectation: mode A (plain) WILL contain it; modes B/C MUST NOT.
    Returns a summary dict for the report.
    """
    needle = FIXED_MESSAGE.encode("ascii")
    hits = [f for f in frames if needle in f["payload"]]
    return {
        "frames_checked": len(frames),
        "plaintext_hits": len(hits),
        "hit_lines": [f["line_no"] for f in hits[:10]],
    }


def analyze_log(path: str, mode: str = "unknown") -> int:
    """Offline analysis: channel/RSSI/len summary + entropy + plaintext check."""
    if not os.path.exists(path):
        print(f"[FAIL] log not found: {path}", file=sys.stderr)
        return 1
    frames = parse_ring_log(path)
    if not frames:
        print(f"[FAIL] no ring-dump lines parsed in {path}")
        print("  Expected lines of form: ts,rssi,ch,len,hex")
        return 1

    channels = Counter(f["ch"] for f in frames)
    lengths = Counter(f["length"] for f in frames)
    rssis = [f["rssi"] for f in frames]
    blob = b"".join(f["payload"] for f in frames)
    entropy = shannon_entropy(blob)
    replay = check_replay_of_plaintext(frames)
    hopping_violation = len(channels) > 1

    print(f"=== promisc_capture_helper analysis: {path} (mode={mode}) ===")
    print(f"  frames parsed : {len(frames)} (target ~{DEFAULT_FRAMES_PER_MODE}/mode)")
    print(f"  channels      : {dict(channels)}"
          + ("  [WARN] hopping left ON?" if hopping_violation else "  [OK] single channel"))
    print(f"  lengths       : {dict(sorted(lengths.items()))}")
    print(f"  rssi dBm      : min {min(rssis)} max {max(rssis)} "
          f"mean {sum(rssis) / len(rssis):.1f}")
    print(f"  entropy       : {entropy:.3f} bits/byte over {len(blob)} payload bytes")
    print(f"  plaintext check ('{FIXED_MESSAGE}'): "
          f"{replay['plaintext_hits']}/{replay['frames_checked']} frames contain vector"
          + (f" lines={replay['hit_lines']}" if replay["hit_lines"] else ""))
    if mode in ("ccmp", "ceepew") and replay["plaintext_hits"]:
        print("  [FAIL] cleartext vector visible in encrypted mode — finding.")
        return 1
    if mode == "plain" and not replay["plaintext_hits"]:
        print("  [WARN] plain mode shows no cleartext vector — check test setup.")
    print("  [PASS] analysis complete.")
    return 0


def run_capture_session(mode: str, port: str, frames: int, channel: int,
                        duration: int, baud: int, log_path: str) -> int:
    """Drive a capture run (host side). DUT config itself is manual/firmware-side.

    This skeleton validates arguments, prints the exact bench checklist, and
    shells to tools/ceepew_monitor.py for the serial log. It does NOT touch
    firmware: hopping off / mode select are operator steps on the DUTs.
    """
    if mode not in MODES:
        print(f"[FAIL] unknown mode {mode!r}; choose from {MODES}", file=sys.stderr)
        return 2
    print("=== promisc_capture_helper capture session ===")
    print(f"  mode    : {mode}")
    print(f"  vector  : {FIXED_MESSAGE}")
    print(f"  frames  : ~{frames} | channel: {channel} (hopping OFF) | port: {port}")
    print("  operator steps BEFORE logging:")
    print(f"    1. Set both DUTs to mode {mode} and fixed channel {channel}.")
    print("    2. Confirm channel hopping DISABLED for this run.")
    print(f"    3. Queue the fixed vector '{FIXED_MESSAGE}' for repeated TX.")
    print(f"  logging : python tools/ceepew_monitor.py --ports {port}"
          f" --baud {baud} --duration {duration} --log {log_path}")
    print("  convert : python tools/capture/serial_to_pcap.py"
          f" {log_path} -o <date>_ch{channel}_{mode}.pcap --radiotap")
    print(f"  analyze : python tools/promisc_capture_helper.py --analyze {log_path}"
          f" --cap-mode {mode}")
    return 0


def self_test() -> int:
    """Deterministic offline self-test (no hardware)."""
    plain_line = f"123456,-42,1,27,{FIXED_MESSAGE.encode().hex()}\n"
    enc_payload = bytes(range(27))  # no ASCII vector inside
    enc_line = f"123500,-45,1,64,{enc_payload.hex()}\n"
    import tempfile

    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as tmp:
        tmp.write("boot text that must be skipped\n")
        tmp.write(plain_line)
        tmp.write(enc_line)
        tmp_path = tmp.name
    try:
        frames = parse_ring_log(tmp_path)
        assert len(frames) == 2, f"expected 2 frames, got {len(frames)}"
        assert frames[0]["ch"] == 1 and frames[0]["rssi"] == -42
        replay = check_replay_of_plaintext(frames)
        assert replay["plaintext_hits"] == 1, replay
        entropy = shannon_entropy(b"".join(f["payload"] for f in frames))
        assert entropy > 0.0
        print("[PASS] promisc_capture_helper self-test (parse + entropy + plaintext check).")
        return 0
    except AssertionError as exc:
        print(f"[FAIL] self-test: {exc}", file=sys.stderr)
        return 1
    finally:
        os.unlink(tmp_path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="3-mode RF promiscuous capture helper (host-only, no firmware edits).")
    parser.add_argument("--mode", choices=list(MODES),
                        help="capture mode: plain / ccmp / ceepew")
    parser.add_argument("--port", default="COM5", help="DUT serial port (default: COM5)")
    parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES_PER_MODE,
                        help=f"frames per mode (default: {DEFAULT_FRAMES_PER_MODE})")
    parser.add_argument("--channel", type=int, default=DEFAULT_CHANNEL,
                        help=f"fixed RF channel, hopping OFF (default: {DEFAULT_CHANNEL})")
    parser.add_argument("--duration", type=int, default=900,
                        help="monitor duration seconds (default: 900)")
    parser.add_argument("--baud", type=int, default=921600,
                        help="capture baud (default: 921600, see capture_session.md)")
    parser.add_argument("--log", default="capture_run1.txt", help="serial log path")
    parser.add_argument("--analyze", metavar="LOG",
                        help="offline analysis of a ring-dump log file")
    parser.add_argument("--cap-mode", default="unknown", choices=list(MODES) + ["unknown"],
                        help="mode label for --analyze plaintext expectation")
    parser.add_argument("--self-test", action="store_true",
                        help="run deterministic offline self-test")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.analyze:
        return analyze_log(args.analyze, mode=args.cap_mode)
    if args.mode:
        return run_capture_session(args.mode, args.port, args.frames,
                                   args.channel, args.duration, args.baud, args.log)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())

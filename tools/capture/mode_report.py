#!/usr/bin/env python3
"""mode_report.py - per-mode OTA evidence report for CEE-PEW E1.

Takes one capture-mode pcap (produced by serial_to_pcap.py from on-node
promiscuous ring dumps) and produces the evidence the E1 spec asks for:

  1. tshark field export - exactly the requested columns, dissected by the
     same engine Wireshark uses, so the numbers are reproducible in the GUI:
         frame.time_epoch, frame.len, radiotap.channel.freq,
         radiotap.dbm_antsignal, wlan.fc.retry, wlan.fc.type_subtype,
         wlan.seq
  2. payload byte-pool entropy and the printable-ASCII share.
  3. cleartext field inventory - does the application message survive to the
     air verbatim in this mode? Reports each known marker and its offset.
  4. a per-mode verdict line: PLAINTEXT VISIBLE / NOT VISIBLE.

Payload offset: ESP-NOW vendor action frames are
     [24 B 802.11 MAC header][1 B category 0x7F][3 B OUI 18:fe:34][payload]
so the ESP-NOW payload starts at offset 28. The script auto-locates the
7F 18 FE 34 signature and records the offset it actually used, so a different
layout can never silently skew the entropy numbers.

Usage
-----
  python tools/capture/mode_report.py data/raw/E1_A_plain_20261017/capture.pcap \
      --mode "A plain (no CCMP, no ESL)" \
      --derived-dir data/derived/E1_A_plain_20261017

  python tools/capture/mode_report.py --self-test
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

try:
    from analyze_pcap import (read_pcap_records, strip_radiotap,  # noqa: E402
                              parse_80211, shannon_entropy)
except ImportError:  # pragma: no cover - import guard for odd CWDs
    read_pcap_records = strip_radiotap = parse_80211 = shannon_entropy = None

ESPNOW_SIG = bytes((0x7F, 0x18, 0xFE, 0x34))
DEFAULT_PAYLOAD_OFFSET = 28

# Application markers we look for verbatim on the air.
#
# The E1 stimulus is the BENCH58 scripted frame: 58 bytes, every byte 0x41
# ('A') except byte [0] which carries the sequence index. 0x41 is a printable
# character, so an unencrypted mode shows a long run of 'A'. The older
# promisc_capture_helper vector is also checked in case it was used.
MARKERS = [
    ("bench58_A_run", re.compile(rb"A{16,}")),
    ("cmd_open_valve", re.compile(rb"CMD=OPEN_VALVE_04")),
    ("auth_admin", re.compile(rb"AUTH=ADMIN")),
]

TSHARK_FIELDS = [
    "frame.time_epoch",
    "frame.len",
    "radiotap.channel.freq",
    "radiotap.dbm_antsignal",
    "wlan.fc.retry",
    "wlan.fc.type_subtype",
    "wlan.seq",
]


def find_tshark(explicit: str | None) -> str | None:
    if explicit and os.path.exists(explicit):
        return explicit
    for cand in (r"C:\Program Files\Wireshark\tshark.exe",
                 r"C:\Program Files (x86)\Wireshark\tshark.exe",
                 "/usr/bin/tshark", "/usr/local/bin/tshark"):
        if os.path.exists(cand):
            return cand
    # PATH lookup
    for d in os.environ.get("PATH", "").split(os.pathsep):
        cand = os.path.join(d, "tshark.exe" if os.name == "nt" else "tshark")
        if os.path.exists(cand):
            return cand
    return None


def tshark_export(tshark: str, pcap: str, out_csv: str, verbose: bool = False) -> int:
    """Run tshark -T fields with exactly the requested columns."""
    cmd = [tshark, "-r", pcap, "-T", "fields",
           "-E", "header=y", "-E", "separator=,", "-E", "occurrence=a",
           "-E", "quote=d"]
    for f in TSHARK_FIELDS:
        cmd += ["-e", f]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"[WARN] tshark failed: {exc}", file=sys.stderr)
        return -1
    if out.returncode != 0:
        print(f"[WARN] tshark exit {out.returncode}: {out.stderr.strip()[:300]}",
              file=sys.stderr)
        return out.returncode
    with open(out_csv, "w", encoding="utf-8", newline="") as fh:
        fh.write(out.stdout)
    rows = max(0, len(out.stdout.strip().splitlines()) - 1)
    if verbose:
        print(f"  tshark: {rows} dissected frame(s) -> {out_csv}")
    return rows


def payload_offset(frame: bytes) -> int | None:
    """Locate the Espressif OUI; ESP-NOW payload starts 4 B after it begins."""
    i = frame.find(ESPNOW_SIG)
    if i < 0:
        return None
    return i + len(ESPNOW_SIG)


def analyse(pcap: str, mode: str, derived_dir: str, tshark: str | None,
            verbose: bool = False) -> int:
    os.makedirs(derived_dir, exist_ok=True)
    network, recs = read_pcap_records(pcap)
    ack_excluded = 0

    frames, offsets, bodies = [], [], []
    for idx, (_ts_sec, _ts_usec, _incl, _orig, pkt) in enumerate(recs):
        ch = None
        rssi = None
        if network == 127:  # DLT_IEEE80211_RADIOTAP
            ch, rssi, pkt = strip_radiotap(pkt)
        if not pkt:
            continue
        # ESP32 HW-ACK / short control marker: not an application frame.
        if pkt[0] == 0x1D:
            ack_excluded += 1
            continue
        info = parse_80211(pkt)
        fc = info.get("fc", 0)
        off = payload_offset(pkt)
        body = pkt[off:] if off is not None else b""
        frames.append({
            "i": idx, "len": len(pkt), "rssi": rssi, "ch": ch,
            "retry": info.get("retry", 0),
            "type": info.get("type", -1),
            "type_subtype": (fc >> 2) & 0xF,
            "seq": info.get("seq"), "off": off, "body_len": len(body),
        })
        if off is not None:
            offsets.append(off)
            bodies.append(body)

    pool = bytearray()
    for b in bodies:
        pool += b

    n = len(frames)
    hist: dict[int, int] = {}
    for f in frames:
        hist[f["len"]] = hist.get(f["len"], 0) + 1

    printable = sum(1 for b in pool if 32 <= b < 127)
    ascii_share = (printable / len(pool)) if pool else 0.0
    ent = shannon_entropy(bytes(pool)) if pool else 0.0

    markers = []
    for name, rex in MARKERS:
        # Search PER FRAME. Searching the concatenation of all bodies would
        # let a run of bytes span a frame boundary and inflate match length,
        # and non-overlapping search would under-count repeated frames.
        hit_frames = []
        first_off = None
        longest = 0
        for fi, body in enumerate(bodies):
            m = rex.search(body)
            if m:
                hit_frames.append(fi)
                longest = max(longest, m.end() - m.start())
                if first_off is None:
                    first_off = (fi, m.start())
        markers.append({
            "marker": name,
            "pattern": rex.pattern.decode("ascii", "replace"),
            "frames_matched": len(hit_frames),
            "frames_total": len(bodies),
            "first_frame": first_off[0] if first_off else None,
            "first_offset_in_frame": first_off[1] if first_off else None,
            "longest_match_bytes": longest,
        })

    retry_n = sum(1 for f in frames if f["retry"] == 1)
    plaintext_visible = any(m["frames_matched"] > 0 for m in markers)

    report = {
        "mode": mode,
        "pcap": pcap,
        "pcap_linktype": network,
        "n_frames": n,
        "hw_ack_excluded": ack_excluded,
        "payload_offset_used": (sorted(set(offsets)) if offsets else None),
        "payload_bytes_total": len(pool),
        "length_histogram": dict(sorted(hist.items())),
        "entropy_bits_per_byte": round(ent, 4),
        "printable_ascii_share": round(ascii_share, 4),
        "retry_frames": retry_n,
        "cleartext_markers": markers,
        "plaintext_visible_on_air": plaintext_visible,
    }

    with open(os.path.join(derived_dir, "mode_report.json"), "w",
              encoding="utf-8") as fh:
        json.dump(report, fh, indent=2)
        fh.write("\n")

    # Human-readable summary
    lines = []
    lines.append("=" * 78)
    lines.append(f"MODE REPORT - {mode}")
    lines.append("=" * 78)
    lines.append(f"pcap                    : {pcap}")
    lines.append(f"frames analysed         : {n}")
    lines.append(f"HW-ACK markers excluded : {ack_excluded}")
    lines.append(f"ESP-NOW payload offset  : {report['payload_offset_used']}")
    lines.append(f"payload bytes pooled    : {len(pool)}")
    lines.append("")
    lines.append("Frame-length histogram (incl. any FCS the driver left in):")
    for ln, c in sorted(hist.items()):
        lines.append(f"  {ln:5d} B : {c:4d}  {'#' * min(c, 60)}")
    lines.append("")
    lines.append(f"Byte entropy of pooled ESP-NOW payload : {ent:.4f} bits/B")
    lines.append(f"Printable-ASCII share of that payload  : {ascii_share*100:.2f}% "
                 f"({printable}/{len(pool)} B)")
    lines.append(f"Retry-flagged frames                   : {retry_n}/{n}")
    lines.append("")
    lines.append("Cleartext field inventory (verbatim search in ESP-NOW payload):")
    for m in markers:
        if m["frames_matched"]:
            lines.append(f"  FOUND   {m['marker']:<16} "
                         f"in {m['frames_matched']}/{m['frames_total']} frames, "
                         f"longest run {m['longest_match_bytes']} B, "
                         f"first in frame {m['first_frame']} "
                         f"@ payload offset {m['first_offset_in_frame']}")
        else:
            lines.append(f"  absent  {m['marker']:<16} pattern "
                         f"{m['pattern']!r} in 0/{m['frames_total']} frames")
    lines.append("")
    lines.append(f"VERDICT: application plaintext is "
                 f"{'VISIBLE' if plaintext_visible else 'NOT VISIBLE'} on air "
                 f"in this mode.")
    text = "\n".join(lines)
    print(text)
    with open(os.path.join(derived_dir, "mode_report.txt"), "w",
              encoding="utf-8") as fh:
        fh.write(text + "\n")

    if tshark:
        r = tshark_export(tshark, pcap,
                          os.path.join(derived_dir, "tshark_fields.csv"), verbose)
        if r >= 0:
            print(f"\ntshark field export: {r} frame(s) -> "
                  f"{os.path.join(derived_dir, 'tshark_fields.csv')}")
    else:
        print("\n[NOTE] tshark not found - field export skipped.", file=sys.stderr)
    return 0


# --------------------------------------------------------------------------
# self-test
# --------------------------------------------------------------------------

def _self_test() -> int:
    import struct
    import tempfile
    fails = []

    def check(name, cond, detail=""):
        print(f"  [{'PASS' if cond else 'FAIL'}] {name} {detail}")
        if not cond:
            fails.append(name)

    RADIOTAP = (struct.pack("<BBHI", 0, 0, 13, 0x28)      # CHANNEL|DBM_ANTSIGNAL
                + struct.pack("<HH", 2412, 0)             # freq ch1, no flags
                + struct.pack("<b", -45))                 # dbm signal

    def mk_pkt(body: bytes) -> bytes:
        # [radiotap 13 B][24 B mgmt hdr][category+OUI 4 B][body]
        hdr = bytes((0xD0, 0x00)) + b"\x00" * 22
        return RADIOTAP + hdr + ESPNOW_SIG + body

    with tempfile.TemporaryDirectory() as d:
        p1 = os.path.join(d, "plain.pcap")
        with open(p1, "wb") as fh:
            fh.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 127))
            body = b"A" * 58
            for i in range(3):
                pkt = mk_pkt(body)
                fh.write(struct.pack("<IIII", 1700000000 + i, 0, len(pkt), len(pkt)))
                fh.write(pkt)

        st = None  # noqa: F841 (kept out of the way; analyse() is called below)
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = analyse(p1, "selftest-plain", os.path.join(d, "derived"), None)
        rep = json.load(open(os.path.join(d, "derived", "mode_report.json")))
        check("rc==0", rc == 0, f"(got {rc})")
        check("3 frames", rep["n_frames"] == 3, f"(got {rep['n_frames']})")
        check("payload offset 28", rep["payload_offset_used"] == [28],
              f"(got {rep['payload_offset_used']})")
        check("low entropy", rep["entropy_bits_per_byte"] < 1.0,
              f"(got {rep['entropy_bits_per_byte']})")
        check("all printable", rep["printable_ascii_share"] == 1.0,
              f"(got {rep['printable_ascii_share']})")
        check("A-run in all 3 frames",
              rep["cleartext_markers"][0]["frames_matched"] == 3,
              f"(got {rep['cleartext_markers'][0]['frames_matched']})")
        check("plaintext VISIBLE", rep["plaintext_visible_on_air"] is True)

        # High-entropy / encrypted-looking mode must report NOT visible.
        # NOTE: empirical entropy is biased low for small samples — n bytes
        # drawn from a 256-symbol source can show at most log2(n) bits/byte.
        # Use enough bytes (8 x 200 B) for the >6.5 assertion to be meaningful.
        import os as _os
        p2 = _os.path.join(d, "enc.pcap")
        with open(p2, "wb") as fh:
            fh.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 127))
            for i in range(8):
                pkt = mk_pkt(_os.urandom(200))
                fh.write(struct.pack("<IIII", 1700000100 + i, 0,
                                     len(pkt), len(pkt)))
                fh.write(pkt)
        with redirect_stdout(io.StringIO()):
            analyse(p2, "selftest-enc", _os.path.join(d, "derived2"), None)
        rep2 = json.load(open(_os.path.join(d, "derived2", "mode_report.json")))
        check("encrypted NOT visible", rep2["plaintext_visible_on_air"] is False)
        check("high entropy", rep2["entropy_bits_per_byte"] > 6.5,
              f"(got {rep2['entropy_bits_per_byte']})")

    print(f"\n{'FAILED: ' + ', '.join(fails) if fails else 'self-test OK'}")
    return 1 if fails else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pcap", nargs="?", help="capture pcap for one mode")
    ap.add_argument("--mode", default="unknown", help="human label for the mode")
    ap.add_argument("--derived-dir", default="data/derived",
                    help="where to write mode_report.json/.txt + tshark csv")
    ap.add_argument("--tshark", default=None, help="path to tshark")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()
    if not args.pcap:
        ap.error("give a pcap path or --self-test")
    if read_pcap_records is None:
        print("[FAIL] cannot import analyze_pcap.py from the same directory",
              file=sys.stderr)
        return 2
    return analyse(args.pcap, args.mode, args.derived_dir,
                   find_tshark(args.tshark), args.verbose)


if __name__ == "__main__":
    sys.exit(main())

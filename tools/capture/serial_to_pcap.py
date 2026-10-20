#!/usr/bin/env python3
"""serial_to_pcap.py — convert on-node promiscuous ring-dump logs to pcap.

Input:  `ts,rssi,ch,len,hex` ring-dump lines from ceepew_monitor.py logs.
        The monitor wraps device output as "<elapsed> [LABEL] <line>", which
        this parser strips automatically. Non-matching lines are skipped and
        counted (use -v to list them).

        Field meanings:
          ts   radio timestamp in microseconds (esp_timer / promiscuous RX
               stamp domain). See --ts-unit if the firmware emits ms or s.
          rssi received signal strength in dBm (negative int).
          ch   Wi-Fi channel (1-14).
          len  declared 802.11 MPDU length in bytes.
          hex  raw frame bytes as hex (whitespace/colons tolerated).

Length / budget verification:
  Full CEE-PEW data frame = 249 B payload + 39 B 802.11/LLC/CEE-PEW header
  = 288 B MPDU (excl FCS). Two checks per line:
    1. LEN_MISMATCH (error): declared `len` != actual decoded hex bytes.
       The actual bytes are written; the line is flagged and counted.
    2. NON_FULL_SIZE (info): actual bytes != 288. Short frames are legal
       (e.g. HW-ACK 0x1d control frames); they are written normally and
       counted separately so the operator can confirm the short frames are
       only the expected ACK subtype.

Output: 802.11 pcap, linktype 105 (IEEE80211, no radiotap) by default.
  Global header: magic 0xa1b2c3d4 (bytes d4 c3 b2 a1 LE), version 2.4,
  snaplen 65535. Per-packet header: ts_sec/ts_usec/incl_len/orig_len with
  incl_len == orig_len always.
  Timestamps: ts_sec/usec = --epoch-base (unix seconds at radio ts=0,
  default: wall-clock time at conversion start) + radio ts.

  With --radiotap, each packet is prefixed with a minimal radiotap header
  (antenna signal + channel frequency/flags) and the global linktype becomes
  127 (IEEE80211_RADIOTAP). RSSI/channel then survive into Wireshark as
  radiotap.dbm_antsignal; without the flag they are kept only in the
  converter's stdout summary, not in the pcap.

Stdlib only. Self-test: `python serial_to_pcap.py --self-test`.
"""

from __future__ import annotations

import argparse
import binascii
import re
import struct
import sys
import time

# Full data-frame budget: 249 B payload + 39 B header = 288 B MPDU excl FCS.
EXPECTED_FULL_MPDU = 288
PAYLOAD_BUDGET = 249
HEADER_BUDGET = 39

DLT_IEEE80211 = 105
DLT_IEEE80211_RADIOTAP = 127

PCAP_GLOBAL_FMT = "<IHHIIII"   # magic, ver_maj, ver_min, zone, sigfigs, snaplen, network
PCAP_REC_FMT = "<IIII"         # ts_sec, ts_usec, incl_len, orig_len
PCAP_MAGIC = 0xA1B2C3D4
PCAP_VERSION = (2, 4)
SNAPLEN = 65535

# Optional "<elapsed> [LABEL]" monitor prefix, then the csv payload.
LINE_RE = re.compile(
    r"^(?:\s*\d+(?:\.\d+)?\s+\[[^\]]+\]\s+)?"  # ceepew_monitor.py wrapper
    r"\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([0-9a-fA-F: \t]+)\s*$"
)

TS_SCALE = {"us": 1, "ms": 1_000, "s": 1_000_000}

# 2.4 GHz channel -> centre frequency MHz.
def channel_to_freq_mhz(ch: int) -> int:
    if ch == 14:
        return 2484
    return 2412 + 5 * (ch - 1)


def build_radiotap(rssi: int, ch: int) -> bytes:
    """Minimal radiotap header: CHANNEL (freq+flags) + DBM_ANTSIGNAL.

    present bitmap bit 3 (0x08) = CHANNEL, bit 5 (0x20) = DBM_ANTSIGNAL.
    Layout: ver(1) pad(1) len(2) present(4) freq(2) flags(2) signal(1).
    Channel flags 0x00a0 = 2.4 GHz | CCK.
    """
    freq = channel_to_freq_mhz(ch)
    signal = max(-128, min(127, rssi))
    # total length 13: 8 base + 4 channel + 1 signal
    return struct.pack("<BBHIHh b", 0, 0, 13, 0x28, freq, 0x00A0, signal)


def parse_line(line: str):
    """Return (ts_raw, rssi, ch, declared_len, frame_bytes) or None if skipped."""
    m = LINE_RE.match(line)
    if not m:
        return None
    ts_raw, rssi, ch, declared, hexs = m.groups()
    cleaned = re.sub(r"[: \t]", "", hexs)
    try:
        frame = binascii.unhexlify(cleaned)
    except (binascii.Error, ValueError):
        return None
    return int(ts_raw), int(rssi), int(ch), int(declared), frame


def convert(lines, pcap_path, epoch_base, ts_unit="us", use_radiotap=False,
            verbose=False):
    """Convert parsed lines to pcap. Returns stats dict."""
    scale = TS_SCALE[ts_unit]
    base_us = int(epoch_base * 1_000_000)
    linktype = DLT_IEEE80211_RADIOTAP if use_radiotap else DLT_IEEE80211
    stats = {
        "parsed": 0, "skipped": 0, "len_mismatch": 0,
        "non_full_size": 0, "bytes_written": 0,
    }
    records = []  # (ts_sec, ts_usec, payload)
    for i, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line:
            continue
        rec = parse_line(line)
        if rec is None:
            stats["skipped"] += 1
            if verbose:
                print(f"[skip  line {i}]: {line[:100]}", file=sys.stderr)
            continue
        ts_raw, rssi, ch, declared, frame = rec
        actual = len(frame)
        if declared != actual:
            stats["len_mismatch"] += 1
            print(f"[LEN_MISMATCH line {i}]: declared len={declared} != "
                  f"actual hex bytes={actual} (ts={ts_raw} ch={ch}); "
                  f"writing actual bytes.", file=sys.stderr)
        if actual != EXPECTED_FULL_MPDU:
            stats["non_full_size"] += 1
            if verbose:
                print(f"[info  line {i}]: {actual} B != full-frame budget "
                      f"{PAYLOAD_BUDGET}+{HEADER_BUDGET}={EXPECTED_FULL_MPDU} B "
                      f"(short/ACK frame?)", file=sys.stderr)
        total_us = base_us + ts_raw * scale
        payload = build_radiotap(rssi, ch) + frame if use_radiotap else frame
        records.append((total_us // 1_000_000, total_us % 1_000_000, payload))
        stats["parsed"] += 1
        stats["bytes_written"] += len(payload)

    with open(pcap_path, "wb") as f:
        f.write(struct.pack(PCAP_GLOBAL_FMT, PCAP_MAGIC,
                            *PCAP_VERSION, 0, 0, SNAPLEN, linktype))
        for ts_sec, ts_usec, payload in records:
            f.write(struct.pack(PCAP_REC_FMT, ts_sec, ts_usec,
                                len(payload), len(payload)))
            f.write(payload)
    stats["linktype"] = linktype
    return stats


def read_pcap_records(pcap_path):
    """Minimal pcap reader for self-test validation. Returns (network, recs)."""
    with open(pcap_path, "rb") as f:
        blob = f.read()
    magic, ver_maj, ver_min, _zone, _sig, _snap, network = struct.unpack(
        PCAP_GLOBAL_FMT, blob[:struct.calcsize(PCAP_GLOBAL_FMT)])
    assert magic == PCAP_MAGIC, f"bad magic {magic:#x}"
    off = struct.calcsize(PCAP_GLOBAL_FMT)
    recs = []
    while off < len(blob):
        ts_sec, ts_usec, incl, orig = struct.unpack(
            PCAP_REC_FMT, blob[off:off + struct.calcsize(PCAP_REC_FMT)])
        off += struct.calcsize(PCAP_REC_FMT)
        recs.append((ts_sec, ts_usec, incl, orig, blob[off:off + incl]))
        off += incl
    return network, recs


def self_test() -> int:
    """Exercise converter on 3 synthetic lines; return 0 on PASS."""
    import os
    import tempfile

    full_hex = ("aa" * EXPECTED_FULL_MPDU)          # 288 B full data frame
    ack_hex = "1d00" + "bb" * 8                     # 10 B HW-ACK-ish short frame
    short_hex = "cc" * 10                          # 10 B but declared 288 -> mismatch
    lines = [
        f"1000000,-45,6,{EXPECTED_FULL_MPDU},{full_hex}",
        f"12.3 [DEVICE_A] 1001500,-50,6,10,{ack_hex}",  # monitor-wrapped
        f"1002000,-60,6,288,{short_hex}",                # LEN_MISMATCH
    ]
    fails = []

    def check(name, cond, detail=""):
        print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}")
        if not cond:
            fails.append(name)

    with tempfile.TemporaryDirectory() as d:
        # Plain 802.11 variant.
        p1 = os.path.join(d, "t1.pcap")
        st = convert(lines, p1, epoch_base=1_700_000_000.0, verbose=False)
        check("parsed==3", st["parsed"] == 3, f"(got {st['parsed']})")
        check("len_mismatch==1", st["len_mismatch"] == 1,
              f"(got {st['len_mismatch']})")
        check("non_full_size==2", st["non_full_size"] == 2,
              f"(got {st['non_full_size']})")
        net, recs = read_pcap_records(p1)
        check("linktype==105", net == DLT_IEEE80211, f"(got {net})")
        check("3 pcap records", len(recs) == 3, f"(got {len(recs)})")
        check("incl==orig all", all(r[2] == r[3] for r in recs))
        check("frame lens",
              [r[2] for r in recs] == [EXPECTED_FULL_MPDU, 10, 10],
              f"(got {[r[2] for r in recs]})")
        check("ts monotonic",
              [(r[0], r[1]) for r in recs] == sorted((r[0], r[1]) for r in recs))
        check("ts base applied", recs[0][0] == 1_700_000_001,
              f"(got ts_sec={recs[0][0]})")

        # Radiotap variant.
        p2 = os.path.join(d, "t2.pcap")
        st2 = convert(lines, p2, epoch_base=1_700_000_000.0, use_radiotap=True)
        net2, recs2 = read_pcap_records(p2)
        check("radiotap linktype==127", net2 == DLT_IEEE80211_RADIOTAP,
              f"(got {net2})")
        check("radiotap prefix len 13",
              all(r[4][:4] == struct.pack("<BBH", 0, 0, 13) for r in recs2))
        check("radiotap incl==orig", all(r[2] == r[3] for r in recs2))

    if fails:
        print(f"SELF-TEST FAILED: {fails}", file=sys.stderr)
        return 1
    print("SELF-TEST: all checks passed (3 synthetic lines).")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Convert ceepew_monitor.py ring-dump logs "
                    "(ts,rssi,ch,len,hex) to 802.11 pcap.")
    ap.add_argument("input", nargs="?", help="input log file (or stdin if omitted)")
    ap.add_argument("-o", "--output", required=False, help="output .pcap path")
    ap.add_argument("--radiotap", action="store_true",
                    help="embed RSSI/channel in radiotap headers (linktype 127)")
    ap.add_argument("--epoch-base", type=float, default=None,
                    help="unix seconds at radio ts=0 (default: now)")
    ap.add_argument("--ts-unit", choices=["us", "ms", "s"], default="us",
                    help="radio timestamp unit (default: us)")
    ap.add_argument("--self-test", action="store_true",
                    help="run built-in self-test on 3 synthetic lines")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="list skipped lines and non-full-size notes")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.output:
        print("error: -o/--output is required (or use --self-test)",
              file=sys.stderr)
        return 2

    epoch_base = args.epoch_base if args.epoch_base is not None else time.time()
    if args.input:
        with open(args.input, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    else:
        lines = sys.stdin.readlines()

    stats = convert(lines, args.output, epoch_base,
                    ts_unit=args.ts_unit, use_radiotap=args.radiotap,
                    verbose=args.verbose)
    print(f"wrote {args.output}: linktype={stats['linktype']} "
          f"parsed={stats['parsed']} skipped={stats['skipped']} "
          f"len_mismatch={stats['len_mismatch']} "
          f"non_full_size={stats['non_full_size']} "
          f"bytes={stats['bytes_written']}")
    return 0 if stats["len_mismatch"] == 0 else 0  # mismatches flagged, not fatal


if __name__ == "__main__":
    sys.exit(main())

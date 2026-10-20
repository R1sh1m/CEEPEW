#!/usr/bin/env python3
"""analyze_pcap.py — analyze 802.11 pcaps produced by serial_to_pcap.py.

Reads linktype-105 (IEEE80211, no radiotap) and linktype-127
(IEEE80211_RADIOTAP, minimal header written by serial_to_pcap.py --radiotap)
pcaps and emits:

  - frame-length histogram (text table + optional CSV via --hist-csv)
  - byte-entropy of payloads (overall + per-frame mean/min/max)
  - per-channel counts (radiotap pcaps only; 105 has no channel info)
  - airtime table reusing the 1 Mbps + 192 us PLCP + 4 B FCS model,
    cross-checked against tools/wireshark/airtime_calc.py reference rows
    (92 B -> 0.928 ms, 277 B -> 2.408 ms, 181 B -> 1.640 ms,
    292 B -> 2.528 ms)
  - wlan.fc.retry clustering note near hop boundaries
    (channel-change / large timestamp-gap / 5 s hop grid)
  - HW-ACK 0x1d exclusion count (frames with first byte 0x1d are counted
    separately and excluded from histogram/entropy/airtime/seq stats)
  - wlan.seq gap accounting (missing sequence numbers, duplicates)

Stdlib only. Self-test: `python analyze_pcap.py --self-test`.

The pcap reader below duplicates serial_to_pcap.py's ~20-line minimal
reader on purpose (do NOT import serial_to_pcap — keep zero deps).
"""

from __future__ import annotations

import argparse
import collections
import csv
import math
import struct
import sys

DLT_IEEE80211 = 105
DLT_IEEE80211_RADIOTAP = 127

PCAP_GLOBAL_FMT = "<IHHIIII"   # magic, ver_maj, ver_min, zone, sigfigs, snaplen, network
PCAP_REC_FMT = "<IIII"         # ts_sec, ts_usec, incl_len, orig_len
PCAP_MAGIC = 0xA1B2C3D4
PCAP_GLOBAL_LEN = struct.calcsize(PCAP_GLOBAL_FMT)
PCAP_REC_LEN = struct.calcsize(PCAP_REC_FMT)

# Airtime model (must match tools/wireshark/airtime_calc.py):
#   airtime_us = 192 + on_air_bytes * 8   (1 Mbps -> 8 us/byte)
PLCP_PREAMBLE_US = 192.0
FCS_BYTES = 4
HOP_DWELL_MS = 5000.0  # ceepew_config.h CEEPEW_HOP_INTERVAL_MS

# (label, MPDU bytes incl FCS, expected airtime ms) — cross-check rows.
AIRTIME_REFS = [
    ("#7", 92, 0.928),
    ("#207", 277, 2.408),
    ("Lite", 181, 1.640),
    ("Full 249+39", 292, 2.528),
]
AIRTIME_TOL_MS = 0.025

HW_ACK_FIRST_BYTE = 0x1D  # ESP32 HW-ACK / short control marker


# --------------------------------------------------------------------------
# Minimal pcap reader (duplicated from serial_to_pcap.py, zero-dep copy).
# --------------------------------------------------------------------------

def read_pcap_records(pcap_path):
    """Return (network, recs) where recs = [(ts_sec, ts_usec, incl, orig, bytes)]."""
    with open(pcap_path, "rb") as f:
        blob = f.read()
    if len(blob) < PCAP_GLOBAL_LEN:
        raise ValueError(f"truncated pcap global header ({len(blob)} B)")
    magic, _vmaj, _vmin, _zone, _sig, _snap, network = struct.unpack(
        PCAP_GLOBAL_FMT, blob[:PCAP_GLOBAL_LEN])
    if magic != PCAP_MAGIC:
        raise ValueError(f"bad pcap magic {magic:#x}")
    if network not in (DLT_IEEE80211, DLT_IEEE80211_RADIOTAP):
        raise ValueError(f"unsupported linktype {network} (need 105 or 127)")
    off = PCAP_GLOBAL_LEN
    recs = []
    while off < len(blob):
        if off + PCAP_REC_LEN > len(blob):
            raise ValueError("truncated pcap record header")
        ts_sec, ts_usec, incl, orig = struct.unpack(
            PCAP_REC_FMT, blob[off:off + PCAP_REC_LEN])
        off += PCAP_REC_LEN
        if off + incl > len(blob):
            raise ValueError("truncated pcap packet data")
        recs.append((ts_sec, ts_usec, incl, orig, blob[off:off + incl]))
        off += incl
    return network, recs


# --------------------------------------------------------------------------
# 802.11 / radiotap parsing helpers
# --------------------------------------------------------------------------

def channel_to_freq_mhz(ch: int) -> int:
    if ch == 14:
        return 2484
    return 2412 + 5 * (ch - 1)


def freq_to_channel(freq: int):
    if freq == 2484:
        return 14
    if 2412 <= freq <= 2472 and (freq - 2412) % 5 == 0:
        return (freq - 2412) // 5 + 1
    return None


def strip_radiotap(pkt: bytes):
    """Split (channel|None, rssi|None, 802.11 bytes) from a linktype-127 packet.

    Walks the radiotap present-bitmap fields in order (TSFT/FLAGS/RATE/
    CHANNEL/FHSS/DBM_ANTSIGNAL) so the CHANNEL offset is correct even if
    earlier optional fields are present. Unknown layouts -> (None, None, pkt
    past it_len) best-effort; garbage it_len -> (None, None, b"").
    """
    if len(pkt) < 8:
        return None, None, b""
    _ver, _pad, it_len, present = struct.unpack("<BBHI", pkt[:8])
    if it_len > len(pkt) or it_len < 8:
        return None, None, b""
    # Ordered walk over the low present bits to find CHANNEL / SIGNAL.
    # (bit, size, alignment)
    fields = [(0, 8, 8), (1, 1, 1), (2, 1, 1), (3, 4, 2), (4, 2, 2), (5, 1, 1)]
    off = 8
    chan_off = None
    sig_off = None
    for bit, size, align in fields:
        if present & (1 << bit):
            if off % align:
                off += align - (off % align)
            if bit == 3:
                chan_off = off
            if bit == 5:
                sig_off = off
            off += size
    ch = None
    rssi = None
    if chan_off is not None and chan_off + 4 <= it_len:
        freq = struct.unpack("<H", pkt[chan_off:chan_off + 2])[0]
        ch = freq_to_channel(freq)
    if sig_off is not None and sig_off < it_len:
        rssi = struct.unpack("<b", pkt[sig_off:sig_off + 1])[0]
    return ch, rssi, pkt[it_len:]


def parse_80211(frame: bytes):
    """Return dict(fc, type, retry, seq|None). seq valid for mgmt/data len>=24."""
    if len(frame) < 2:
        return {"fc": 0, "type": -1, "retry": 0, "seq": None}
    (fc,) = struct.unpack("<H", frame[:2])
    ftype = (fc >> 2) & 0x3
    retry = (fc >> 11) & 0x1
    seq = None
    if ftype in (0, 2) and len(frame) >= 24:
        (sc,) = struct.unpack("<H", frame[22:24])
        seq = (sc >> 4) & 0xFFF
    return {"fc": fc, "type": ftype, "retry": retry, "seq": seq}


def airtime_ms(length_bytes: int, fcs_included: bool = True) -> float:
    on_air = length_bytes if fcs_included else length_bytes + FCS_BYTES
    return (PLCP_PREAMBLE_US + on_air * 8.0) / 1000.0


def shannon_entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = collections.Counter(data)
    n = len(data)
    return -sum((c / n) * math.log2(c / n) for c in counts.values())


# --------------------------------------------------------------------------
# Analysis
# --------------------------------------------------------------------------

def analyze(recs, network, gap_ms=1000):
    """Core analysis over pcap records. Returns a stats dict."""
    frames = []  # (idx, ts_us, ch, len, retry, seq, ftype, bytes)
    ack_excluded = 0
    for i, (ts_sec, ts_usec, _incl, _orig, pkt) in enumerate(recs):
        ts_us = ts_sec * 1_000_000 + ts_usec
        ch = None
        if network == DLT_IEEE80211_RADIOTAP:
            ch, _rssi, pkt = strip_radiotap(pkt)
        if len(pkt) >= 1 and pkt[0] == HW_ACK_FIRST_BYTE:
            ack_excluded += 1
            continue
        info = parse_80211(pkt)
        frames.append((i, ts_us, ch, len(pkt), info["retry"], info["seq"],
                       info["type"], pkt))

    hist = collections.Counter(f[3] for f in frames)
    per_channel = collections.Counter(
        f[2] for f in frames if f[2] is not None)
    chan_unknown = sum(1 for f in frames if f[2] is None)

    # Entropy: overall byte pool + per-frame distribution.
    pool = bytearray()
    per_frame_ent = []
    for f in frames:
        per_frame_ent.append(shannon_entropy(f[7]))
        pool.extend(f[7])
    overall_ent = shannon_entropy(bytes(pool))

    # Retry accounting.
    retry_idx = [n for n, f in enumerate(frames) if f[4] == 1]
    retry_count = len(retry_idx)

    # Hop boundaries: channel changes + large ts gaps. 5 s grid from first
    # frame is used as the assumed hop schedule when channels are unknown.
    boundaries = []  # list of (kind, frame_pos)
    for n in range(1, len(frames)):
        prev_ch, cur_ch = frames[n - 1][2], frames[n][2]
        if prev_ch is not None and cur_ch is not None and cur_ch != prev_ch:
            boundaries.append(("chan-change", n))
    gap_events_ts = []
    if frames:
        for n in range(1, len(frames)):
            if frames[n][1] - frames[n - 1][1] > gap_ms * 1000:
                boundaries.append(("ts-gap", n))
                gap_events_ts.append(n)
    grid_hits = []
    if frames:
        # grid boundary positions: first frame index in each new 5 s slot
        seen = set()
        for n, f in enumerate(frames):
            slot = (f[1] - frames[0][1]) // int(HOP_DWELL_MS * 1000)
            if slot not in seen:
                seen.add(slot)
                if n > 0:
                    grid_hits.append(n)
    edge_set = set(b for _, b in boundaries) | set(grid_hits)

    edge_window = 5  # frames
    near_edge = sum(1 for r in retry_idx
                    if any(abs(r - b) <= edge_window for b in edge_set))

    # Sequence-gap accounting (capture order, non-ACK frames with seq).
    seq_gaps = 0
    seq_missing = 0
    seq_dups = 0
    seq_ok = 0
    seq_na = 0
    gap_list = []  # (pos, prev_seq, cur_seq, missing)
    prev_seq = None
    for n, f in enumerate(frames):
        s = f[5]
        if s is None:
            seq_na += 1
            continue
        if prev_seq is None:
            prev_seq = s
            continue
        delta = (s - prev_seq) % 4096
        if delta == 0:
            seq_dups += 1
        elif delta == 1:
            seq_ok += 1
        else:
            seq_gaps += 1
            seq_missing += delta - 1
            gap_list.append((n, prev_seq, s, delta - 1))
        prev_seq = s

    # Airtime cross-check vs airtime_calc.py reference rows.
    airtime_check = []
    for label, nbytes, expect_ms in AIRTIME_REFS:
        got = airtime_ms(nbytes, fcs_included=True)
        airtime_check.append((label, nbytes, got, expect_ms,
                              abs(got - expect_ms) <= AIRTIME_TOL_MS))

    # Per-length airtime table for observed lengths.
    air_rows = sorted(hist.items())

    return {
        "total_records": len(recs),
        "analyzed": len(frames),
        "ack_excluded": ack_excluded,
        "hist": hist,
        "per_channel": per_channel,
        "chan_unknown": chan_unknown,
        "has_channel": network == DLT_IEEE80211_RADIOTAP,
        "overall_entropy": overall_ent,
        "per_frame_entropy": per_frame_ent,
        "retry_count": retry_count,
        "retry_idx": retry_idx,
        "boundaries": boundaries,
        "grid_hits": grid_hits,
        "near_edge": near_edge,
        "edge_window": edge_window,
        "seq_gaps": seq_gaps,
        "seq_missing": seq_missing,
        "seq_dups": seq_dups,
        "seq_ok": seq_ok,
        "seq_na": seq_na,
        "gap_list": gap_list,
        "airtime_check": airtime_check,
        "air_rows": air_rows,
        "frames": frames,
    }


def args_gap_ms_default() -> int:
    return 1000


def report(st, network) -> str:
    L = []
    L.append(f"linktype: {network} "
             f"({'IEEE80211_RADIOTAP' if network == 127 else 'IEEE80211'})")
    L.append(f"records: total={st['total_records']} analyzed={st['analyzed']} "
             f"HW-ACK-0x1d-excluded={st['ack_excluded']}")
    L.append("")
    L.append("Frame-length histogram (non-ACK frames):")
    L.append(f"{'len(B)':>7} {'count':>7} {'airtime_ms*':>11}")
    for ln, c in sorted(st["hist"].items()):
        L.append(f"{ln:>7} {c:>7} {airtime_ms(ln):>11.3f}")
    L.append("* airtime @1Mbps +192us PLCP, length assumed incl 4B FCS "
             "(+0.032ms if excl).")
    L.append("")
    if st["has_channel"]:
        L.append("Per-channel counts (radiotap):")
        for ch, c in sorted(st["per_channel"].items()):
            L.append(f"  ch{ch}: {c}")
        if st["chan_unknown"]:
            L.append(f"  unknown: {st['chan_unknown']}")
    else:
        L.append("Per-channel counts: n/a — linktype 105 carries no "
                 "radiotap/channel info (use --radiotap capture).")
    L.append("")
    pf = st["per_frame_entropy"]
    if pf:
        L.append(f"Byte entropy: overall={st['overall_entropy']:.3f} bits/B "
                 f"(n={sum(st['hist'][k] * k for k in st['hist'])} payload B); "
                 f"per-frame mean={sum(pf)/len(pf):.3f} "
                 f"min={min(pf):.3f} max={max(pf):.3f}")
    else:
        L.append("Byte entropy: n/a (no analyzed frames)")
    L.append("")
    L.append("Airtime cross-check (must match airtime_calc.py):")
    allpass = True
    for label, nbytes, got, expect, ok in st["airtime_check"]:
        L.append(f"  [{'PASS' if ok else 'FAIL'}] {label}: {nbytes}B -> "
                 f"{got:.3f} ms (expect {expect:.3f})")
        allpass = allpass and ok
    L.append(f"  => {'all match' if allpass else 'MISMATCH — model drift!'}")
    L.append("")
    L.append(f"Retry (wlan.fc.retry==1): {st['retry_count']}/{st['analyzed']} "
             f"frames; {st['near_edge']} within {st['edge_window']} frames "
             f"of a hop boundary.")
    L.append(f"  boundaries: {len(st['boundaries'])} ts/chan events + "
             f"{len(st['grid_hits'])} 5s-grid slots.")
    L.append("  NOTE: retries clustering within the edge window of hop "
             "boundaries (chan-change / >1s ts-gap / 5s grid) suggest "
             "hop-loss ARQ; uniformly spread retries suggest random loss. "
             "Confirm with tshark `wlan.fc.retry==1` + channel column.")
    L.append("")
    L.append(f"Seq gaps (wlan.seq, capture order): ok_steps={st['seq_ok']} "
             f"gap_events={st['seq_gaps']} missing={st['seq_missing']} "
             f"dups={st['seq_dups']} no_seq={st['seq_na']}")
    for pos, prev, cur, miss in st["gap_list"][:10]:
        L.append(f"  frame#{pos}: seq {prev}->{cur} (missing {miss})")
    if len(st["gap_list"]) > 10:
        L.append(f"  ... +{len(st['gap_list']) - 10} more")
    return "\n".join(L)


# --------------------------------------------------------------------------
# Self-test: 5-frame synthetic pcap
# --------------------------------------------------------------------------

def _make_data_frame(seq: int, total_len: int, retry: bool = False) -> bytes:
    fc0 = 0x08  # type DATA (10), subtype 0
    fc1 = 0x08 if retry else 0x00  # bit3 of byte1 == retry (fc bit 11)
    hdr = struct.pack("<BBH", fc0, fc1, 0)  # FC + duration
    hdr += b"\x00" * 18  # addr1/2/3 (6*3)
    hdr += struct.pack("<H", (seq << 4) & 0xFFF0)  # seq control, frag 0
    assert len(hdr) == 24
    body_len = total_len - len(hdr)
    body = bytes((i * 37 + seq) % 256 for i in range(body_len))
    return hdr + body


def _write_pcap(path, packets, linktype):
    with open(path, "wb") as f:
        f.write(struct.pack(PCAP_GLOBAL_FMT, PCAP_MAGIC, 2, 4, 0, 0,
                            65535, linktype))
        ts = 1_700_000_000_000_000
        for pkt in packets:
            f.write(struct.pack(PCAP_REC_FMT, ts // 1_000_000,
                                ts % 1_000_000, len(pkt), len(pkt)))
            f.write(pkt)
            ts += 200_000  # 200 ms spacing (no ts-gap boundaries)


def self_test() -> int:
    import os
    import tempfile

    fails = []

    def check(name, cond, detail=""):
        print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}")
        if not cond:
            fails.append(name)

    # 5-frame synthetic capture:
    #   F0 data seq10 92B | F1 data seq11 92B | F2 data seq13 277B (gap: 12)
    #   F3 retry dup seq13 277B | F4 HW-ACK 0x1d 10B (excluded)
    pkts = [
        _make_data_frame(10, 92),
        _make_data_frame(11, 92),
        _make_data_frame(13, 277),
        _make_data_frame(13, 277, retry=True),
        bytes([HW_ACK_FIRST_BYTE, 0x00]) + b"\xbb" * 8,
    ]
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "synth105.pcap")
        _write_pcap(p, pkts, DLT_IEEE80211)
        net, recs = read_pcap_records(p)
        check("linktype==105", net == 105, f"(got {net})")
        check("5 pcap records", len(recs) == 5, f"(got {len(recs)})")
        st = analyze(recs, net)
        check("total==5", st["total_records"] == 5)
        check("ACK-excluded==1", st["ack_excluded"] == 1,
              f"(got {st['ack_excluded']})")
        check("analyzed==4", st["analyzed"] == 4,
              f"(got {st['analyzed']})")
        check("histogram {92:2,277:2}", dict(st["hist"]) == {92: 2, 277: 2},
              f"(got {dict(st['hist'])})")
        check("retry==1", st["retry_count"] == 1,
              f"(got {st['retry_count']})")
        check("seq missing==1 (seq12)", st["seq_missing"] == 1,
              f"(got {st['seq_missing']})")
        check("seq gap_events==1", st["seq_gaps"] == 1,
              f"(got {st['seq_gaps']})")
        check("seq dups==1 (retry)", st["seq_dups"] == 1,
              f"(got {st['seq_dups']})")
        check("entropy 0<e<=8",
              0.0 < st["overall_entropy"] <= 8.0,
              f"(got {st['overall_entropy']:.3f})")
        check("airtime refs all PASS",
              all(r[4] for r in st["airtime_check"]),
              f"(got {[round(r[2],3) for r in st['airtime_check']]})")
        check("ch unknown for 105",
              (not st["has_channel"]) and st["chan_unknown"] == 4,
              f"(got unknown={st['chan_unknown']})")

        # Radiotap rewrap bonus: same 5 frames on ch6 -> per-channel {6:5}.
        rt_pkts = []
        for pkt in pkts:
            freq = channel_to_freq_mhz(6)
            rt = struct.pack("<BBHIHh b", 0, 0, 13, 0x28, freq, 0x00A0, -45)
            rt_pkts.append(rt + pkt)
        p2 = os.path.join(d, "synth127.pcap")
        _write_pcap(p2, rt_pkts, DLT_IEEE80211_RADIOTAP)
        net2, recs2 = read_pcap_records(p2)
        st2 = analyze(recs2, net2)
        check("radiotap ch6==4 (non-ACK)", dict(st2["per_channel"]) == {6: 4},
              f"(got {dict(st2['per_channel'])})")
        check("radiotap ACK-excluded==1", st2["ack_excluded"] == 1)

        # CSV smoke: histogram round-trips through csv module.
        hp = os.path.join(d, "hist.csv")
        with open(hp, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["len_bytes", "count", "airtime_ms"])
            for ln, c in sorted(st["hist"].items()):
                w.writerow([ln, c, f"{airtime_ms(ln):.3f}"])
        with open(hp) as f:
            rows = list(csv.DictReader(f))
        check("hist csv 2 rows", len(rows) == 2, f"(got {len(rows)})")

    if fails:
        print(f"SELF-TEST FAILED: {fails}", file=sys.stderr)
        return 1
    print("SELF-TEST: all checks passed (5-frame synthetic pcap).")
    return 0


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Analyze 802.11 pcaps from serial_to_pcap.py "
                    "(linktype 105/127).")
    ap.add_argument("input", nargs="?",
                    help="input .pcap path (omitted with --self-test)")
    ap.add_argument("--hist-csv", metavar="PATH",
                    help="write frame-length histogram CSV here")
    ap.add_argument("--gap-ms", type=int, default=args_gap_ms_default(),
                    help="timestamp gap treated as hop boundary "
                         "(default: 1000 ms)")
    ap.add_argument("--self-test", action="store_true",
                    help="run built-in self-test on a 5-frame synthetic pcap")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.input:
        print("error: input .pcap required (or use --self-test)",
              file=sys.stderr)
        return 2

    try:
        network, recs = read_pcap_records(args.input)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if not recs:
        print("error: no packets in pcap", file=sys.stderr)
        return 1

    st = analyze(recs, network, gap_ms=args.gap_ms)
    print(report(st, network))
    print(f"[info] hop-boundary ts-gap threshold: {args.gap_ms} ms "
          f"(default {args_gap_ms_default()} ms)")

    if args.hist_csv:
        try:
            with open(args.hist_csv, "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["len_bytes", "count", "airtime_ms"])
                for ln, c in sorted(st["hist"].items()):
                    w.writerow([ln, c, f"{airtime_ms(ln):.3f}"])
            print(f"wrote histogram CSV: {args.hist_csv}")
        except OSError as e:
            print(f"error writing CSV: {e}", file=sys.stderr)
            return 1

    if not all(r[4] for r in st["airtime_check"]):
        print("AIRTIME CROSS-CHECK MISMATCH", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

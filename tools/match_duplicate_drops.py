#!/usr/bin/env python3
"""CEE-PEW duplicate-drop matcher (replay-bitmap drops vs lost-MAC-ACK hypothesis).

For each replay-bitmap drop in serial logs, matches:
  1. a prior RX accept of the same 8-bit wire seq on the same device, and
  2. the peer's TX of the same seq plus sender-side retry evidence:
     an espnow_send_cb FAIL chain (lost MAC-ACK -> sender retransmit ->
     duplicate -> REPLAY reject) OR a repeated peer ARQ_TX of the same
     seq (original + retransmit; the only retry signal in older
     firmware logs that carry no FAIL lines).

Usage:
    python tools/match_duplicate_drops.py
    python tools/match_duplicate_drops.py --files logs/frag2_capture.txt
    python tools/match_duplicate_drops.py --window 2000 --csv /tmp/drops.csv

Exit code is always 0; verdicts are in the printed summary table.
"""

from __future__ import annotations

import argparse
import csv
import glob
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PATTERNS = ("logs/*.txt", "logs/*.log")

device_re = re.compile(r"\[(DEVICE_[AB]|COM\d+)\]")
stamp_re = re.compile(r"\[(?:DEVICE_[AB]|COM\d+)\]\s+[IEWDB]\s+\((\d+)\)")
replay_re = re.compile(
    r"\[ARQ_RX\] REPLAY reject seq=0x([0-9A-Fa-f]{1,4})"
    r" expected_byte=0x([0-9A-Fa-f]{1,4}) expected=(\d+) diff=(-?\d+)"
)
dup_re = re.compile(
    r"\[SECURE_CHAT_RX\] ARQ decode failed / duplicate sequence \(err=(\d+)\)"
)
accept_re = re.compile(
    r"\[ARQ_RX\] (?:first-frame )?accept seq=0x([0-9A-Fa-f]{1,4})"
)
tx_re = re.compile(r"\[ARQ_TX\] len=(\d+) seq=0x([0-9A-Fa-f]{1,4})")
fail_re = re.compile(r"espnow_send_cb: FAIL")


@dataclass
class log_line:
    lineno: int
    device: str
    stamp_ms: int | None
    text: str


@dataclass
class drop_record:
    source_file: str
    drop_lineno: int
    device: str
    seq: int | None
    seq_hex: str
    prior_rx_lineno: int | None = None
    peer_tx_lineno: int | None = None
    peer_device: str = ""
    fail_count: int = 0
    fail_first_lineno: int | None = None
    retry_tx_count: int = 0
    retry_first_lineno: int | None = None
    verdict: str = "unmatched"
    reason: str = ""


def parse_log_file(path: Path) -> list[log_line]:
    lines: list[log_line] = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for lineno, raw in enumerate(handle, start=1):
            match = device_re.search(raw)
            device = match.group(1) if match else "unknown"
            stamp_match = stamp_re.search(raw)
            stamp_ms = int(stamp_match.group(1)) if stamp_match else None
            lines.append(log_line(
                lineno=lineno, device=device, stamp_ms=stamp_ms,
                text=raw.rstrip("\n")))
    return lines


def find_peer_device(lines: list[log_line], drop_device: str,
                     seq: int, drop_lineno: int) -> str:
    candidates: dict[str, int] = {}
    for entry in lines:
        if entry.lineno >= drop_lineno:
            break
        if entry.device == drop_device or entry.device == "unknown":
            continue
        tx_match = tx_re.search(entry.text)
        if tx_match and int(tx_match.group(2), 16) == seq:
            candidates[entry.device] = entry.lineno
    if candidates:
        return max(candidates, key=lambda dev: candidates[dev])
    others: dict[str, int] = {}
    for entry in lines:
        if entry.device != drop_device and entry.device != "unknown":
            others[entry.device] = others.get(entry.device, 0) + 1
    if others:
        return max(others, key=lambda dev: others[dev])
    return ""


def match_drops_in_file(path: Path, window: int,
                        slack_ms: int) -> list[drop_record]:
    lines = parse_log_file(path)
    records: list[drop_record] = []
    for idx, entry in enumerate(lines):
        if not dup_re.search(entry.text):
            continue
        seq: int | None = None
        drop_expected: int | None = None
        # The REPLAY detail line normally sits 0-3 lines above the drop.
        for back in range(0, 4):
            if idx - back < 0:
                break
            replay_match = replay_re.search(lines[idx - back].text)
            if (replay_match
                    and lines[idx - back].device == entry.device):
                seq = int(replay_match.group(1), 16)
                drop_expected = int(replay_match.group(3))
                break
        record = drop_record(
            source_file=path.name,
            drop_lineno=entry.lineno,
            device=entry.device,
            seq=seq,
            seq_hex=f"0x{seq:02X}" if seq is not None else "n/a",
        )
        if seq is None:
            record.reason = "no-seq-detail"
            records.append(record)
            continue
        assert drop_expected is not None
        # 1. Prior RX accept of the same seq on the same device.
        for prev in range(idx - 1, -1, -1):
            prev_entry = lines[prev]
            if prev_entry.device != entry.device:
                continue
            accept_match = accept_re.search(prev_entry.text)
            if (accept_match
                    and int(accept_match.group(1), 16) == seq):
                record.prior_rx_lineno = prev_entry.lineno
                break
        if record.prior_rx_lineno is None:
            # Late/stale frame: the window already advanced past this seq
            # via a jump-accept of a higher seq (ARQ accepts diff > 0 and
            # skips unseen seqs, which then read as REPLAY on late arrival).
            advanced = False
            for prev in range(idx - 1, -1, -1):
                prev_entry = lines[prev]
                if prev_entry.device != entry.device:
                    continue
                accept_match = accept_re.search(prev_entry.text)
                if accept_match:
                    other = int(accept_match.group(1), 16)
                    if (other - seq) % 256 < 128 and other != seq:
                        advanced = True
                        break
            record.reason = ("stale-late-frame"
                             if advanced else "no-prior-rx")
            if not advanced:
                records.append(record)
                continue
            # Stale-late-frame still needs a sender-side retry chain to
            # count as retransmit-attributed; fall through to step 2 with
            # prior_rx left unset.
        # 2. Peer TX of the same seq before the drop.
        peer = find_peer_device(lines, entry.device, seq, entry.lineno)
        record.peer_device = peer
        peer_tx_linenos: list[int] = []
        peer_tx_stamp: int | None = None
        for prev in range(idx - 1, -1, -1):
            prev_entry = lines[prev]
            if peer and prev_entry.device != peer:
                continue
            if prev_entry.device == entry.device:
                continue
            tx_match = tx_re.search(prev_entry.text)
            if tx_match and int(tx_match.group(2), 16) == seq:
                peer_tx_linenos.append(prev_entry.lineno)
                if peer_tx_stamp is None:
                    peer_tx_stamp = prev_entry.stamp_ms
        peer_tx_linenos.sort()
        peer_tx_lineno = peer_tx_linenos[-1] if peer_tx_linenos else None
        record.peer_tx_lineno = peer_tx_lineno
        record.retry_tx_count = len(peer_tx_linenos)
        record.retry_first_lineno = (
            peer_tx_linenos[0] if peer_tx_linenos else None)
        if peer_tx_lineno is None:
            record.reason = "no-peer-tx"
            records.append(record)
            continue
        # 3. TX-FAIL chain on the SENDER side between the peer TX and the
        # drop (timestamps tolerate dual-port multiplex interleave).
        fail_lines: list[int] = []
        for prev_entry in lines:
            if (prev_entry.device != peer
                    or not fail_re.search(prev_entry.text)):
                continue
            if peer_tx_stamp is not None and entry.stamp_ms is not None \
                    and prev_entry.stamp_ms is not None:
                if peer_tx_stamp <= prev_entry.stamp_ms <= \
                        entry.stamp_ms + slack_ms:
                    fail_lines.append(prev_entry.lineno)
            elif peer_tx_lineno <= prev_entry.lineno <= \
                    entry.lineno + 50:
                fail_lines.append(prev_entry.lineno)
        # Fall back to a bounded line window when timestamps are missing.
        if not fail_lines and (peer_tx_stamp is None
                               or entry.stamp_ms is None):
            lo = max(peer_tx_lineno, entry.lineno - window)
            fail_lines = [
                prev_entry.lineno
                for prev_entry in lines
                if lo <= prev_entry.lineno <= entry.lineno + 50
                and prev_entry.device == peer
                and fail_re.search(prev_entry.text)
            ]
        record.fail_count = len(fail_lines)
        record.fail_first_lineno = fail_lines[0] if fail_lines else None
        # Sender-side retry evidence is EITHER an espnow_send_cb FAIL
        # chain (lost MAC-ACK) OR a repeated peer ARQ_TX of the same seq
        # (original + retransmit). Older firmware logs carry no FAIL
        # lines, so the reTX count is the primary retry signal there.
        # A stale-late-frame (window already advanced, no prior RX of
        # this seq) cannot count as retransmit-dedup: require prior RX.
        has_prior_rx = record.prior_rx_lineno is not None
        has_retry = bool(fail_lines) or record.retry_tx_count >= 2
        if has_prior_rx and has_retry:
            record.verdict = "matched"
            record.reason = ""
        elif not has_prior_rx and record.reason in ("", "stale-late-frame"):
            record.reason = "stale-late-frame"
        elif not has_retry:
            record.reason = "no-tx-fail-retry"
        else:
            record.reason = "no-prior-rx"
        records.append(record)
    return records


def format_table(records: list[drop_record]) -> str:
    header = ("| # | file | drop line | dev | seq | prior-RX line | "
              "peer TX line (dev) | FAIL xN @line | verdict | reason |")
    sep = ("|---|------|-----------|-----|-----|---------------|---------------------|"
           "--------------|---------|--------|")
    rows = [header, sep]
    for pos, record in enumerate(records, start=1):
        prior_rx = (str(record.prior_rx_lineno)
                    if record.prior_rx_lineno is not None else "-")
        if record.peer_tx_lineno is not None:
            peer_tx = (f"{record.peer_tx_lineno} "
                       f"({record.peer_device or '?'})")
        else:
            peer_tx = "-"
        retry_bits: list[str] = []
        if record.fail_count:
            retry_bits.append(
                f"FAIL x{record.fail_count} @{record.fail_first_lineno}")
        if record.retry_tx_count >= 2:
            retry_bits.append(f"reTX x{record.retry_tx_count} "
                              f"@{record.retry_first_lineno}")
        elif record.retry_tx_count == 1:
            retry_bits.append("reTX x1 (single)")
        fail = " + ".join(retry_bits) if retry_bits else "-"
        rows.append(
            f"| {pos} | {record.source_file} | {record.drop_lineno} | "
            f"{record.device} | {record.seq_hex} | {prior_rx} | {peer_tx} | "
            f"{fail} | {record.verdict} | {record.reason or '-'} |")
    return "\n".join(rows)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Match replay-bitmap drops to prior-RX + retry chains.")
    parser.add_argument("--files", nargs="*", default=[],
                        help="explicit log files (default: logs/*.txt+*.log)")
    parser.add_argument("--logs", nargs="*", default=list(DEFAULT_PATTERNS),
                        help="glob patterns relative to repo root")
    parser.add_argument("--window", type=int, default=2000,
                        help="FAIL look-back window in lines (default 2000)")
    parser.add_argument("--slack-ms", type=int, default=2000,
                        help="timestamp slack after drop for sender FAIL "
                             "match, dual-port interleave (default 2000 ms)")
    parser.add_argument("--csv", default="",
                        help="optional path for CSV export of per-drop table")
    parser.add_argument("--sample", type=int, default=5,
                        help="evenly-spaced seq-attributed drops re-checked "
                             "for the generalization verdict (default 5)")
    parser.add_argument("--expect-matched", type=int, default=57,
                        help="paper section V-B/VI bar for lost-MAC-ACK "
                             "retransmit-dedup matches (default 57)")
    parser.add_argument("--expect-unmatched", type=int, default=8,
                        help="paper section V-B/VI allowance for unmatched "
                             "drops (default 8)")
    args = parser.parse_args(argv)

    if args.files:
        paths = [Path(f) for f in args.files]
    else:
        expanded: list[Path] = []
        for pattern in args.logs:
            expanded.extend(Path(p) for p in
                            glob.glob(str(REPO_ROOT / pattern)))
        paths = sorted(set(expanded))
    paths = [p for p in paths if p.is_file()]
    if not paths:
        print("no log files matched", file=sys.stderr)
        return 0

    records: list[drop_record] = []
    for path in paths:
        records.extend(match_drops_in_file(path, args.window,
                                           args.slack_ms))

    print(f"# duplicate-drop match report ({len(paths)} files, "
          f"window={args.window})")
    print()
    if records:
        print(format_table(records))
    else:
        print("no duplicate-sequence drops found")
    print()

    total = len(records)
    matched = sum(1 for r in records if r.verdict == "matched")
    unmatched = total - matched
    reasons: dict[str, int] = {}
    for record in records:
        if record.verdict != "matched":
            reasons[record.reason or "unknown"] = (
                reasons.get(record.reason or "unknown", 0) + 1)
    print("## summary")
    print(f"- drop events: {total}")
    print(f"- matched (prior-RX + peer-TX same seq + TX-FAIL/retry): "
          f"{matched}")
    print(f"- unmatched: {unmatched}")
    for reason, count in sorted(reasons.items()):
        print(f"  - {reason}: {count}")
    if total:
        print(f"- match rate: {100.0 * matched / total:.1f}%")
    print()

    seq_attributed = [r for r in records if r.seq is not None]
    sample_n = min(args.sample, len(seq_attributed))
    sample: list[drop_record] = (
        [seq_attributed[(i * len(seq_attributed)) // sample_n]
         for i in range(sample_n)] if sample_n else [])
    sample_matched = sum(1 for r in sample if r.verdict == "matched")
    print(f"## sampled-pair check "
          f"({sample_matched}/{sample_n} sampled seq-attributed "
          f"drops matched)")
    for record in sample:
        print(f"- {record.source_file}:{record.drop_lineno} "
              f"seq={record.seq_hex} "
              f"prior_rx={record.prior_rx_lineno or '-'} "
              f"peer_tx={record.peer_tx_lineno or '-'}"
              f"({record.peer_device or '?'}) "
              f"fail_x{record.fail_count} -> {record.verdict}")
    if sample_n < args.sample:
        print(f"- verdict: INSUFFICIENT DATA — only {len(seq_attributed)} "
              f"seq-attributed drops found, need {args.sample}; "
              f"claim needs downgrade to observed counts "
              f"(matched={matched}, unmatched={unmatched}).")
    elif (sample_matched == args.sample
            and matched >= args.expect_matched
            and unmatched <= args.expect_unmatched):
        print(f"- verdict: GENERALIZES — {args.sample}/{args.sample} "
              f"sampled pairs matched and full-log counts "
              f"(matched={matched} >= {args.expect_matched}, "
              f"unmatched={unmatched} <= {args.expect_unmatched}) meet "
              f"paper sections V-B/VI "
              f"(>= {args.expect_matched} lost-MAC-ACK retransmit dedup "
              f"+ {args.expect_unmatched} unmatched).")
    else:
        print(f"- verdict: CLAIM NEEDS DOWNGRADE — sampled "
              f"{sample_matched}/{args.sample} matched, full-log "
              f"matched={matched} (bar {args.expect_matched}), "
              f"unmatched={unmatched} (allowance "
              f"{args.expect_unmatched}); restate paper sections V-B/VI "
              f"with these observed counts.")

    if args.csv:
        with open(args.csv, "w", newline="",
                   encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(["file", "drop_line", "device", "seq",
                             "prior_rx_line", "peer_tx_line", "peer_device",
                             "fail_count", "fail_first_line",
                             "retry_tx_count", "retry_first_line",
                             "verdict", "reason"])
            for record in records:
                writer.writerow([record.source_file, record.drop_lineno,
                                 record.device, record.seq_hex,
                                 record.prior_rx_lineno or "",
                                 record.peer_tx_lineno or "",
                                 record.peer_device, record.fail_count,
                                 record.fail_first_lineno or "",
                                 record.retry_tx_count,
                                 record.retry_first_lineno or "",
                                 record.verdict, record.reason])
        print(f"- csv: {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

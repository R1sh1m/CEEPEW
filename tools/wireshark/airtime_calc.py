#!/usr/bin/env python3
"""CEE-PEW paper airtime calculator — single source for every paper airtime.

Model: 802.11b long PLCP preamble (192 us @ 1 Mbps) + MPDU @ 1 Mbps.
    airtime_us = 192 + (bytes_on_air * 8)      # 1 Mbps -> 8 us per byte

Inputs are frame bytes INCL or EXCL the 4 B FCS; the converter adds the FCS
when excluded so both capture conventions give the same on-air result.

Audit refs (read-only, no firmware edits):
  - 249 B + 39 B frame model (paper main.tex:94, Table IV):
    Full data frame = 249 B payload + 39 B 802.11/LLC/CEE-PEW header = 288 B
    MPDU excl FCS (292 B on air incl FCS).
  - 288 B + FCS airtime line (paper main.tex:94): regenerated below.
  - Hop 9 ch / 5 s dwell (ceepew_config.h:253-255: CEEPEW_HOP_CHANNELS 9U,
    CEEPEW_HOP_INTERVAL_MS 5000U): dwell budget column shows how many such
    frames fit per 5 s dwell (5000 ms / airtime).
  - ARQ retry-bit plan: retries are flagged in captures with the tshark
    display filter ``wlan.fc.retry == 1``; retry frames reuse the same
    airtime row as their original (see RETRY_FILTER below).

Regeneration targets (paper § evaluation):
  - frame #7  (Wireshark frame no.) : ~= 0.93 ms FCS-in
  - frame #207 (Wireshark frame no.): ~= 2.41 ms check
  - Lite-mode frame                 : ~= 1.64 ms
"""

from __future__ import annotations

import argparse
import sys

PLCP_PREAMBLE_US = 192.0   # long-preamble PLCP @ 1 Mbps
RATE_MBPS = 1.0            # basic rate used for management/Full frames
FCS_BYTES = 4              # 802.11 FCS appended on air
HOP_DWELL_MS = 5000.0      # ceepew_config.h: CEEPEW_HOP_INTERVAL_MS

# tshark display-filter plan for ARQ retry accounting (capture-side only).
RETRY_FILTER = "wlan.fc.retry == 1"


def airtime_us(length_bytes: int, fcs_included: bool) -> float:
    """Return on-air time in microseconds.

    Args:
        length_bytes: measured frame bytes, incl or excl the 4 B FCS.
        fcs_included: True if length_bytes already contains the FCS.
    """
    on_air = length_bytes if fcs_included else length_bytes + FCS_BYTES
    return PLCP_PREAMBLE_US + on_air * 8.0 / RATE_MBPS


# Reference frames: (label, bytes, fcs_included, expected_us or None).
# #7 / #207 are Wireshark frame numbers from the paper capture;
# sizes are the measured MPDU lengths that reproduce the paper airtimes.
FRAMES = [
    ("#7", 92, True, 930.0),        # expect ~= 0.93 ms FCS-in
    ("#207", 277, True, 2410.0),    # 2.41 ms check
    ("Lite", 181, True, 1640.0),    # Lite ~= 1.64 ms
    ("Full 249+39 excl FCS", 288, False, None),  # 288 B + FCS line, main.tex:94
    ("Full 249+39 incl FCS", 292, True, None),
]

TOL_US = 25.0  # pass tolerance vs paper expectations (~0.025 ms)


def paper_table() -> str:
    lines = [
        "| Frame | MPDU excl FCS (B) | On-air incl FCS (B) | Airtime (ms) |",
        "|---|---|---|---|",
    ]
    for label, n, fcs_in, _ in FRAMES:
        excl = n if not fcs_in else n - FCS_BYTES
        incl = n if fcs_in else n + FCS_BYTES
        t_ms = airtime_us(n, fcs_in) / 1000.0
        dwell = HOP_DWELL_MS / t_ms
        lines.append(f"| {label} | {excl} | {incl} | {t_ms:.3f} |")
        _ = dwell  # dwell budget available via --dwell
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="CEE-PEW paper airtime calculator")
    ap.add_argument("--csv", action="store_true", help="CSV instead of markdown")
    ap.add_argument("--dwell", action="store_true",
                    help="also show frames-per-5s-dwell budget (9 ch hop)")
    args = ap.parse_args(argv)

    fails = []
    rows = []
    for label, n, fcs_in, expect in FRAMES:
        t = airtime_us(n, fcs_in)
        excl = n if not fcs_in else n - FCS_BYTES
        incl = n if fcs_in else n + FCS_BYTES
        status = "n/a"
        if expect is not None:
            ok = abs(t - expect) <= TOL_US
            status = "PASS" if ok else "FAIL"
            if not ok:
                fails.append(label)
        rows.append((label, excl, incl, t / 1000.0, status))

    if args.csv:
        print("frame,mpdu_excl_B,onair_incl_B,airtime_ms,check")
        for label, excl, incl, ms, st in rows:
            print(f"{label},{excl},{incl},{ms:.3f},{st}")
    else:
        print(paper_table())
        print()
        for label, excl, incl, ms, st in rows:
            extra = ""
            if args.dwell:
                extra = f"  dwell_cap={HOP_DWELL_MS / ms:.1f} frames/5s"
            print(f"[{st}] {label}: excl={excl} B incl={incl} B "
                  f"airtime={ms:.3f} ms{extra}")
        print(f"\nRetry accounting filter (capture plan): {RETRY_FILTER}")

    if fails:
        print(f"FAILED: {', '.join(fails)}", file=sys.stderr)
        return 1
    print("All paper airtime checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

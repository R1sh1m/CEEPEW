#!/usr/bin/env python3
"""CEE-PEW Table IV efficiency recurrence (paper Eqs 20-23), closed form.

Eq (20)  chunk count:   n(P, c) = ceil(P / c)
Eq (21)  base wire:     B(P, c) = P + n * O_mode + H_mode
Eq (22)  FEC total:     T(P, c) = B(P, c) + n * Q_mode + K_mode
         (Q/K are the FEC parity-share per chunk / fixed codebook cost;
         zero for non-FEC modes, so T = B there.)
Eq (23)  efficiency:    eta(P, c) = P / T(P, c)

Table IV mode constants (per-chunk overhead O, fixed H, FEC Q/K):
  MODE            O    H    Q    K      # rationale
  Full+FEC      112   18   15  615     # 112 B base hdr + 15 B parity share;
                                       # 18 B ARQ setup + 615 B parity matrix
  Full no-FEC   112   18    0    0
  Lite           18   22    0    0     # 18 B compact hdr, 22 B setup

Evaluated at the 58 B chunk size AND the 255 B cap for bulk payload
P_BULK = 1340 B, plus each mode's amortized reference exchange at 58 B:
  Lite        P=697  (n=13): T = 697 + 13*18 + 22 = 953
  Full no-FEC P=345  (n=6):  T = 345 + 6*112 + 18 = 1035
  Full+FEC    P=200  (n=4):  T = 200 + 4*(112+15) + (18+615) = 1341

Expected paper results:
  - Full+FEC  @ 255 B cap: ~= 49 %
  - Full no-FEC @ 255 B cap: ~= 66 %
  - Lite bulk check: 1340/1470 = 91.2 %
  - Amortized (58 B chunk refs): 953 / 1035 / 1341 B on air.

Audit refs (read-only, no firmware edits):
  - 249 B + 39 B frame model (paper main.tex:94, Table IV): the O=112 B
    Full-mode per-chunk overhead scales with the 39 B/frame header model
    plus encapsulation across the chunked transfer.
  - 288 B + FCS airtime line (paper main.tex:94): per-frame on-air cost
    feeding the airtime side; see tools/wireshark/airtime_calc.py.
  - Hop 9 ch / 5 s (ceepew_config.h:253-255): efficiency gains directly
    extend useful dwell throughput per 5 s channel stay.
  - ARQ retry-bit plan (``wlan.fc.retry``): Eq (21) counts first attempts;
    retry overhead is measured capture-side, not modelled here.
"""

from __future__ import annotations

import math
import sys

# Eq (20): chunk count for payload P with chunk cap c.
def eq20_nchunks(payload: int, chunk_cap: int) -> int:
    return math.ceil(payload / chunk_cap)


# Eq (21): base wire bytes (payload + per-chunk overhead + fixed setup).
def eq21_base_wire(payload: int, n: int, o_mode: int, h_mode: int) -> int:
    return payload + n * o_mode + h_mode


# Eq (22): FEC total (base + parity share per chunk + fixed codebook).
def eq22_fec_total(base: int, n: int, q_mode: int, k_mode: int) -> int:
    return base + n * q_mode + k_mode


# Eq (23): efficiency = useful payload / total on-air bytes.
def eq23_efficiency(payload: int, total: int) -> float:
    return payload / total


MODES = {
    "Full+FEC": {"O": 112, "H": 18, "Q": 15, "K": 615},
    "Full no-FEC": {"O": 112, "H": 18, "Q": 0, "K": 0},
    "Lite": {"O": 18, "H": 22, "Q": 0, "K": 0},
}

P_BULK = 1340          # bulk reference payload (B)
CHUNK_SMALL = 58       # small-chunk operating point (B)
CHUNK_CAP = 255        # 255 B cap operating point (B)

# Amortized reference exchanges at the 58 B chunk size: mode -> payload.
AMORT_REFS = {"Lite": 697, "Full no-FEC": 345, "Full+FEC": 200}
AMORT_EXPECT = {"Lite": 953, "Full no-FEC": 1035, "Full+FEC": 1341}

# Paper expectations at the 255 B cap (fraction).
EFF_EXPECT = {"Full+FEC": 0.490, "Full no-FEC": 0.660, "Lite": 1340 / 1470}
EFF_TOL = 0.006  # +-0.6 pp


def evaluate(payload: int, chunk: int, mode: str):
    m = MODES[mode]
    n = eq20_nchunks(payload, chunk)
    base = eq21_base_wire(payload, n, m["O"], m["H"])
    total = eq22_fec_total(base, n, m["Q"], m["K"])
    return n, total, eq23_efficiency(payload, total)


def main() -> int:
    fails = []

    print(f"| Mode | n @58 B | eta @58 B | n @255 B | eta @255 B | T @255 B |")
    print("|---|---|---|---|---|---|")
    for mode in ("Full+FEC", "Full no-FEC", "Lite"):
        n58, t58, e58 = evaluate(P_BULK, CHUNK_SMALL, mode)
        n255, t255, e255 = evaluate(P_BULK, CHUNK_CAP, mode)
        print(f"| {mode} | {n58} | {e58*100:.1f} % | {n255} | "
              f"{e255*100:.1f} % | {t255} |")
        if abs(e255 - EFF_EXPECT[mode]) > EFF_TOL:
            fails.append(f"{mode} eta@255 ({e255*100:.2f} %)")
    print()

    # 91.2 % Lite check, verbatim paper ratio.
    lite_ratio = 1340 / 1470
    print(f"Lite check: 1340/1470 = {lite_ratio*100:.2f} % "
          f"(expect 91.2 %) "
          f"[{'PASS' if abs(lite_ratio - 0.912) < 0.001 else 'FAIL'}]")
    if abs(lite_ratio - 0.912) >= 0.001:
        fails.append("1340/1470 check")

    # Amortized reference exchanges at the 58 B chunk size.
    for mode, p in AMORT_REFS.items():
        n, total, _ = evaluate(p, CHUNK_SMALL, mode)
        want = AMORT_EXPECT[mode]
        ok = total == want
        print(f"[{'PASS' if ok else 'FAIL'}] amortized {mode}: "
              f"P={p} n={n} T={total} (expect {want})")
        if not ok:
            fails.append(f"amortized {mode}")

    if fails:
        print(f"FAILED: {', '.join(fails)}", file=sys.stderr)
        return 1
    print("All Table IV efficiency checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

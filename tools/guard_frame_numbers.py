"""Fail if the shipping-frame arithmetic regresses to the double-counted set.

The publication gate treats an arithmetic disagreement as a warning, not a
failure, because one legitimate exception exists (medians are not additive).
That means a revert to the superseded 191 B / 2.064 ms / 30.4 % set still
reaches "AUTHORISED", which is exactly what happened twice during the revision
when a second editor rewrote the passage.

This guard is deliberately narrow. It does not recompute anything; the gate
does that against results/registry.json. It only asserts that the two numbers
which belong to the double-counted frame are absent, and that the three numbers
which belong to the corrected frame are present. A number that appears zero
times has been deleted from the prose; a forbidden number that appears at all
means the 4 B FCS is being counted twice again.

Counts are reported, not asserted, so ordinary edits that mention 187 more or
fewer times do not trip the guard.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TEX = ROOT / "docs" / "paper" / "The Paper" / "main.tex"

# Superseded: 187 B + 4 B of FCS counted a second time, and the airtime and
# efficiency derived from that larger frame.
FORBIDDEN = ("2.064", "30.4")

# Corrected: WB-FULL-NOFEC (187 B), AIR-187-MPDU (2.032 ms), EFF-FULL-NOFEC
# (31.0 %). Each must appear at least once; zero means the correction was lost.
REQUIRED = ("187", "2.032", "31.0")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tex", default=str(DEFAULT_TEX), help="manuscript path")
    args = parser.parse_args()

    path = Path(args.tex)
    if not path.is_file():
        print(f"FAIL: no manuscript at {path}")
        return 1

    text = path.read_text(encoding="utf-8", errors="replace")
    counts = {token: text.count(token) for token in FORBIDDEN + REQUIRED}

    for token in FORBIDDEN + REQUIRED:
        print(f"  {token:<7} {counts[token]}")

    problems = []
    for token in FORBIDDEN:
        if counts[token]:
            problems.append(
                f"superseded value {token!r} appears {counts[token]}x; the 4 B FCS "
                "is being double-counted again"
            )
    for token in REQUIRED:
        if not counts[token]:
            problems.append(f"corrected value {token!r} is absent; the correction was lost")

    if problems:
        print("")
        for problem in problems:
            print(f"FAIL: {problem}")
        print("")
        print("Expected: 187 B frame, 2.032 ms airtime, 31.0 % efficiency.")
        return 1

    print("")
    print("OK: shipping-frame arithmetic is the corrected set (187 B / 2.032 ms / 31.0 %).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

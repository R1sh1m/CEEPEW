#!/usr/bin/env python3
"""tools/bench_prepare_config.py — prepare a CEE-PEW capture-bench build config.

Writes the sdkconfig for one over-the-air capture bench mode so the firmware
build is reproducible and self-describing. Does NOT build or flash.

Bench modes (main/Kconfig.projbuild -> CEEPEW_CAPTURE_LINK_MODE):

  plain   Mode A  CEEPEW_CAPTURE_PEER_NO_LMK=1, CEEPEW_CAPTURE_ESL_BYPASS=1
                  -> cleartext ESP-NOW vendor frames, no CCMP, no ESL/Ascon.
  ccmp    Mode B  CEEPEW_CAPTURE_PEER_NO_LMK=0, CEEPEW_CAPTURE_ESL_BYPASS=1
                  -> hardware CCMP only (industry-standard baseline).
  ceepew  Mode C  CEEPEW_CAPTURE_PEER_NO_LMK=1, CEEPEW_CAPTURE_ESL_BYPASS=0
                  -> no CCMP, ESL/Ascon application crypto ACTIVE.

Note: leaving the link-mode choice UNSET is NOT equivalent to any of the above.
With no mode symbol the two helpers in ceepew_config.h fall through to their
`#else` arms (NO_LMK=0 AND ESL_BYPASS=0), i.e. CCMP *plus* ESL — an
undocumented fourth configuration. This tool always writes exactly one mode.

Usage:
  python tools/bench_prepare_config.py --mode plain  --dir build_cap_plain
  python tools/bench_prepare_config.py --mode ccmp   --dir build_cap_ccmp
  python tools/bench_prepare_config.py --mode ceepew --dir build_cap_ceepew
  python tools/bench_prepare_config.py --mode ceepew --dir build_cap_ceepew --bench58
  python tools/bench_prepare_config.py --verify --dir build_cap_plain
"""

from __future__ import annotations

import argparse
import os
import re
import sys

MODE_SYMS = [
    "CEEPEW_CAPTURE_MODE_PLAIN",
    "CEEPEW_CAPTURE_MODE_CCMP",
    "CEEPEW_CAPTURE_MODE_CEEPEW",
]
MODE_SYM = {
    "plain": "CEEPEW_CAPTURE_MODE_PLAIN",
    "ccmp": "CEEPEW_CAPTURE_MODE_CCMP",
    "ceepew": "CEEPEW_CAPTURE_MODE_CEEPEW",
}
# Which helper values each mode produces (docs/assertion table, not a knob).
MODE_HELPERS = {
    "plain": {"CEEPEW_CAPTURE_PEER_NO_LMK": 1, "CEEPEW_CAPTURE_ESL_BYPASS": 1},
    "ccmp": {"CEEPEW_CAPTURE_PEER_NO_LMK": 0, "CEEPEW_CAPTURE_ESL_BYPASS": 1},
    "ceepew": {"CEEPEW_CAPTURE_PEER_NO_LMK": 1, "CEEPEW_CAPTURE_ESL_BYPASS": 0},
}

_SYM_RE_CACHE: dict[str, re.Pattern] = {}


def _sym_re(sym: str) -> re.Pattern:
    if sym not in _SYM_RE_CACHE:
        _SYM_RE_CACHE[sym] = re.compile(
            r"^(?:# )?CONFIG_" + re.escape(sym) + r"(?: is not set|=[ymn0-9\-]+)?$"
        )
    return _SYM_RE_CACHE[sym]


def set_sym(text: str, sym: str, val: str) -> str:
    """Set CONFIG_<sym>=<val>, collapsing pre-existing duplicates."""
    out, seen = [], False
    for ln in text.splitlines():
        if _sym_re(sym).match(ln.strip()):
            if not seen:
                out.append(f"CONFIG_{sym}={val}")
                seen = True
        else:
            out.append(ln)
    if not seen:
        out.append(f"CONFIG_{sym}={val}")
    return "\n".join(out) + "\n"


def unset_sym(text: str, sym: str) -> str:
    """Force `# CONFIG_<sym> is not set` (choice-member normal form)."""
    out, seen = [], False
    for ln in text.splitlines():
        if _sym_re(sym).match(ln.strip()):
            if not seen:
                out.append(f"# CONFIG_{sym} is not set")
                seen = True
        else:
            out.append(ln)
    if not seen:
        out.append(f"# CONFIG_{sym} is not set")
    return "\n".join(out) + "\n"


def prepare(mode: str, build_dir: str, bench58: bool, seed: str | None) -> int:
    cfg_path = os.path.join(build_dir, "sdkconfig")
    if seed and os.path.exists(seed):
        text = open(seed, encoding="utf-8").read()
        print(f"seeded from {seed}")
    elif os.path.exists(cfg_path):
        text = open(cfg_path, encoding="utf-8").read()
    else:
        print(f"[FAIL] no seed sdkconfig at {cfg_path}; pass --seed", file=sys.stderr)
        return 2

    text = set_sym(text, "CEEPEW_CAPTURE", "y")
    for sym in MODE_SYMS:
        if sym == MODE_SYM[mode]:
            text = set_sym(text, sym, "y")
            text = re.sub(rf"^# CONFIG_{sym} is not set$", f"CONFIG_{sym}=y",
                          text, flags=re.M)
        else:
            text = unset_sym(text, sym)
    text = set_sym(text, "CEEPEW_BENCH_58B", "y" if bench58 else "n")
    if not bench58:
        text = re.sub(r"^CONFIG_CEEPEW_BENCH_58B=n$",
                      "# CONFIG_CEEPEW_BENCH_58B is not set", text, flags=re.M)

    os.makedirs(build_dir, exist_ok=True)
    with open(cfg_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)

    print(f"mode={mode} build_dir={build_dir} bench58={bench58}")
    helpers = MODE_HELPERS[mode]
    for k, v in helpers.items():
        print(f"  expects {k}={v} (derived in ceepew_config.h)")
    for ln in text.splitlines():
        if "CEEPEW_CAPTURE" in ln or "CEEPEW_BENCH" in ln:
            print("   ", ln)
    return 0


def verify(build_dir: str) -> int:
    """Read the GENERATED sdkconfig.h and report the effective mode."""
    gen = os.path.join(build_dir, "config", "sdkconfig.h")
    if not os.path.exists(gen):
        print(f"[FAIL] no generated header at {gen} (build the dir first)",
              file=sys.stderr)
        return 2
    defined = set()
    for ln in open(gen, encoding="utf-8", errors="replace"):
        m = re.match(r"#define CONFIG_(\S+) 1$", ln.strip())
        if m:
            defined.add(m.group(1))
    print(f"== {gen} ==")
    if "CEEPEW_CAPTURE" not in defined:
        print("  normal build (CONFIG_CEEPEW_CAPTURE not set)")
        return 0
    found = [m for m in ("plain", "ccmp", "ceepew")
             if MODE_SYM[m] in defined]
    if "CEEPEW_BENCH_58B" in defined:
        print("  BENCH58 campaign: ENABLED")
    if len(found) != 1:
        print(f"  [FAIL] link mode count={len(found)} {found} — "
              f"expected exactly 1; unset => CCMP+ESL (undocumented)")
        return 1
    mode = found[0]
    print(f"  link mode: {mode}")
    for k, v in MODE_HELPERS[mode].items():
        print(f"  helper {k}={v}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mode", choices=sorted(MODE_SYM), help="bench link mode")
    ap.add_argument("--dir", help="build directory to write sdkconfig into")
    ap.add_argument("--bench58", action="store_true",
                    help="enable the scripted 58 B Full-stack TX campaign")
    ap.add_argument("--seed", help="seed sdkconfig to copy settings from")
    ap.add_argument("--verify", action="store_true",
                    help="verify a built dir's generated sdkconfig.h")
    args = ap.parse_args(argv)

    if args.verify:
        if not args.dir:
            ap.error("--verify requires --dir")
        return verify(args.dir)
    if not args.mode or not args.dir:
        ap.error("--mode and --dir are required")
    return prepare(args.mode, args.dir, args.bench58, args.seed)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Banned-token lint. Implementation: the global ``paperqa`` package.

This file is a compatibility shim. The check itself lives in
``~/.config/opencode/pipelines/paperqa`` so the same rules run in every
project. The printed report and the exit status are unchanged.

One deliberate difference from the pre-shim version: tokens are matched on word
boundaries, so ``pending`` no longer fires inside ``depending``, and the count
agrees with the one ``publication_gate`` enforces.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

INSTALL_HINT = (
    "ERROR: the paperqa package is not installed.\n"
    "Install it with:\n"
    '    uv tool install --editable "$HOME/.config/opencode/pipelines/paperqa"\n'
    "or point PAPERQA_HOME at the package checkout.\n"
)

PAPERQA_HOME = Path(
    os.environ.get(
        "PAPERQA_HOME",
        Path.home() / ".config" / "opencode" / "pipelines" / "paperqa",
    )
)
def _bootstrap() -> None:
    """Make the shared package importable from any interpreter."""
    try:
        import paperqa  # noqa: F401 # type: ignore
        return
    except ImportError:
        pass
    src = PAPERQA_HOME / "src"
    if not src.is_dir():  # pragma: no cover - environment guard
        sys.stderr.write(INSTALL_HINT)
        raise SystemExit(2)
    sys.path.insert(0, str(src))


_bootstrap()

try:
    from paperqa import lint as _lint  # type: ignore
    from paperqa import textstats as _stats  # type: ignore
except ImportError:
    sys.stderr.write(INSTALL_HINT)
    raise SystemExit(2)

# Public name kept: publication_gate.py and older scripts import it from here.
BANNED = list(_lint.CEEPEW_BANNED)

WORDS_PER_PAGE = 900
EM_DASH_LIMIT = 2


def main(path: str) -> int:
    text = _stats.read_text(path)
    found = 0
    for word in BANNED:
        count = _stats.count_word(text, word)
        if count:
            print("BANNED  %40r  count=%d" % (word, count))
            found += 1

    paragraphs = [p for p in text.split("\n\n") if p.strip()]
    pages = max(1, len(text.split()) // WORDS_PER_PAGE)
    em_dashes = text.count("\u2014") + text.count("---")
    print("em dashes per page: %.1f  (limit: %d)" % (em_dashes / pages, EM_DASH_LIMIT))

    lengths = [len(p.split()) for p in paragraphs]
    if lengths:
        print("paragraphs: %d  min/max words: %d/%d"
              % (len(lengths), min(lengths), max(lengths)))
    for i, p in enumerate(paragraphs):
        if p.count("(") > 3:
            print("many parentheses in paragraph %d" % (i + 1))

    if found == 0:
        print("PASS: no banned tokens found")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print("Usage: python tools/paper_lint.py <latex_or_text_file>")
        sys.exit(0 if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help") else 1)
    sys.exit(main(sys.argv[1]))

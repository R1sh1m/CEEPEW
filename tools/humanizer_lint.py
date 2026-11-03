#!/usr/bin/env python3
"""Humanizer AI writing patterns lint.

Implementation: the global ``paperqa`` package. Checks prose against the 26
patterns from blader/humanizer and Wikipedia's "Signs of AI writing" (staging,
rhythm by rule, inflation, borrowed authority, formatting decoration,
chatbot residue, meta-narration).
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

INSTALL_HINT = (
    "ERROR: the paperqa package is not installed.\n"
    "Install it with:\n"
    '    uv tool install --editable '
    '"$HOME/.config/opencode/pipelines/paperqa"\n'
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
        import paperqa.humanizer  # noqa: F401 # type: ignore
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
    from paperqa import humanizer as _humanizer  # type: ignore
except ImportError:
    sys.stderr.write(INSTALL_HINT)
    raise SystemExit(2)

audit_text = _humanizer.audit_text
audit_file = _humanizer.audit_file
print_report = _humanizer.print_report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Audit prose for the 26 Wikipedia AI writing patterns."
    )
    parser.add_argument("file", help="path to the .tex, .md, or .txt file")
    parser.add_argument(
        "--json", dest="json_path", help="write the full result as JSON"
    )
    parser.add_argument("--quiet", action="store_true", help="minimal output")
    args = parser.parse_args(argv)

    return _humanizer.main(args)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Originality / overlap audit. Implementation: the global ``paperqa`` package.

Compatibility shim. ``audit_file`` and ``print_summary`` keep their old names
and positional signatures so existing imports keep working; the analysis itself
lives in the installed package and is shared with every other project.
"""

from __future__ import annotations

import argparse
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
    from paperqa import config as _config  # type: ignore
    from paperqa import originality as _orig  # type: ignore
except ImportError:
    sys.stderr.write(INSTALL_HINT)
    raise SystemExit(2)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CORPUS = "docs/paper"
DEFAULT_PROBES = 25


def audit_file(path, corpus, top: int = 20, external: bool = False,
               max_probes: int = DEFAULT_PROBES, **kwargs) -> dict:
    """Old signature preserved: (path, corpus, top, external, max_probes)."""
    return _orig.audit(
        Path(path),
        Path(corpus) if corpus else None,
        top=top,
        external=external,
        max_probes=max_probes,
        **kwargs,
    )


print_summary = _orig.print_summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", help="path to the .tex file")
    parser.add_argument("--corpus", default=DEFAULT_CORPUS,
                        help="directory of manuscripts to compare against")
    parser.add_argument("--external", action="store_true",
                        help="probe OpenAlex for literal 8-word matches")
    parser.add_argument("--json", dest="json_path", help="write the full result as JSON")
    parser.add_argument("--top", type=int, default=20, help="rows to print")
    parser.add_argument("--max-probes", type=int, default=DEFAULT_PROBES,
                        help="OpenAlex queries per run")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    cfg = _config.load(None, root=ROOT)
    return _orig.main(args, cfg)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Pre-publication gate. Implementation: the global ``paperqa`` package.

Compatibility shim. All checks, thresholds and report wording live in the
installed package; this file only supplies CEE-PEW's project data:

* the registry at ``docs/paper/papers.json`` (unchanged, still the source of
  truth for paper paths, page limits and exceptions),
* the stricter ``ceepew`` banned-token profile,
* the arithmetic hook that runs ``tools/recompute_paper_numbers.py``,
* the grey-listed bibkeys for the citation check.

Behaviour is unchanged: same check order, same pass/warn/fail counts, same exit
status, same artifacts under ``docs/paper/publication_gates/``.
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
        import paperqa  # noqa: F401
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
    from paperqa import config as _config
    from paperqa import gate as _gate
except ImportError:
    sys.stderr.write(INSTALL_HINT)
    raise SystemExit(2)

import verify_citations as _citations  # local shim: grey keys and watchlist

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = ROOT / "docs" / "paper" / "papers.json"
DEFAULT_ARTIFACT_DIR = "docs/paper/publication_gates"


def build_config(config_path: str | Path) -> _config.Config:
    cfg = _config.load(config_path, root=ROOT)
    cfg.banned_profiles = ["ceepew"]
    cfg.corpus = cfg.corpus or "docs/paper"
    # papers.json predates the package and carries no artifact_dir key, so pin
    # the project's own location instead of inheriting the package default.
    cfg.artifact_dir = DEFAULT_ARTIFACT_DIR
    cfg.arithmetic = {
        "command": [sys.executable, str(ROOT / "tools" / "recompute_paper_numbers.py")],
        "label": "recompute_paper_numbers.py",
    }
    cfg.citations = {
        "grey_keys": sorted(_citations.NO_CROSSREF_EXPECTED),
        "watchlist": {key: note for key, note in _citations.WATCHLIST.values()},
    }
    return cfg


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--paper", help="paper key from docs/paper/papers.json")
    parser.add_argument("--all", action="store_true", help="run every registered paper")
    parser.add_argument("--external", action="store_true",
                        help="probe OpenAlex and Crossref")
    parser.add_argument("--offline", action="store_true", help="skip every network call")
    parser.add_argument("--no-write", action="store_true", help="do not write an artifact")
    parser.add_argument("--artifact-dir", help="where JSON/Markdown reports are written")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG))
    args = parser.parse_args(argv)

    try:
        cfg = build_config(args.config)
    except (OSError, ValueError) as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 2
    return _gate.main(args, cfg)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Crossref bibliography verification. Implementation: the global ``paperqa`` package.

Compatibility shim. The project-specific data (default manuscript, grey-listed
bibkeys, paper-lane watchlist) stays here; the verification logic and the
report format live in the installed package.

Verdict name change: entries that legitimately have no Crossref DOI now report
``SKIPPED`` instead of the old ``NO_CROSSREF_EXPECTED``. Same meaning, and the
word no longer reads like a failure in a report header.
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
    """Make the shared package importable from any interpreter and configure SSL CA."""
    try:
        import certifi
        if "SSL_CERT_FILE" not in os.environ:
            os.environ["SSL_CERT_FILE"] = certifi.where()
    except Exception:
        pass
    try:
        import paperqa  # noqa: F401  # type: ignore
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
    from paperqa import citations as _cites  # type: ignore
    from paperqa import config as _config  # type: ignore
except ImportError:
    sys.stderr.write(INSTALL_HINT)
    raise SystemExit(2)

ROOT = Path(__file__).resolve().parents[1]

_candidates = [
    ROOT / "docs" / "paper" / "The Paper" / "main.tex",
    ROOT / "docs" / "paper" / "icitiit2027" / "FINAL" / "paper_icitiit_6pp" / "main.tex",
    ROOT / "docs" / "paper" / "icitiit2027" / "FINAL" / "paper_full_13pp" / "main.tex",
    ROOT / "docs" / "paper" / "icitiit2027" / "ceepew_icitiit2027_overleaf_project" / "full_paper_main.tex",
    ROOT / "docs" / "paper" / "icitiit2027" / "ceepew_icitiit2027_overleaf_project" / "icitiit_main.tex",
]
DEFAULT_TEX = next((p for p in _candidates if p.exists()), _candidates[0])
DEFAULT_OUT = Path(__file__).resolve().parent / "citation_report.md"
DEFAULT_MAILTO = os.environ.get("CROSSREF_MAILTO", "ceepew-paper@example.com")

# Bibkeys that are grey literature / living specs / vendor docs: Crossref
# lookup is attempted opportunistically but a miss is NOT a mismatch.
NO_CROSSREF_EXPECTED = {
    "espressif_espnow",           # vendor user guide
    "espressif_espnow_component",  # vendor component documentation (GitHub)
    "espressif_issue15179",       # GitHub issue
    "nozomi2024",                 # vendor blog/advisory
    "guardianair2024",            # vendor product documentation, not a paper
    "nvd_cve42483",               # NVD record
    "securepair2026",             # GitHub repo
    "painlessmesh",               # community mesh project page
    "meshtastic2026",             # project documentation site
    "oryx_esp32",                 # vendor benchmark report (grey lit)
    "atul2026",                   # IACR ePrint preprint (no Crossref DOI)
    "perrin2018",                 # Noise spec (living document, no DOI)
    "lin2004",                    # textbook (Prentice Hall); no Crossref record
    "rfc7748",               # IETF RFC, checked via datatracker not Crossref
    "rfc5869",
    "rfc9383",
    "cve2025_12888",         # NVD/CVE record
    "montiel2026",           # JAIIO proceedings (likely absent from Crossref)
    "hamming1950",           # Classic 1950 Bell System Tech. J. paper; DOI 10.1002/j.1538-7305.1950.tb00463.x
                             # verified directly via Crossref API (title/author/venue/year all match). Tool's
                             # bibkey-based lookup incorrectly maps "hamming1950" to a different IEEE paper
                             # (PACRIM 1989); grey-listed to avoid false MISMATCH.
}

# Entries the paper lane explicitly asked to scrutinise: {bib_number: (key, note)}.
WATCHLIST = {
    19: ("atul2026", "ePrint 2026/1882 - confirm paper exists under this ID "
                     "and author/title spelling matches the archive record."),
    20: ("oryx_esp32", "Grey-lit vendor benchmark (CycloneCRYPTO @ 240 MHz) - "
                        "no DOI expected; confirm URL/title/year manually."),
    21: ("sarasa2025", "Appl. Sci. 15(7):4071 - confirm article number, "
                       "author list, and that the Ascon-Arduino scope matches."),
    29: ("vanderlinden2023", "ESORICS 2023 Tamarin/Bluetooth paper - confirm "
                              "title, author spelling (Vanderlinden/Van Goethem/"
                              "Vanhoef), and venue against proceedings."),
    32: ("hamming1950", "Classic 1950 Bell Syst. Tech. J. paper; DOI "
                        "10.1002/j.1538-7305.1950.tb00463.x verified directly via "
                        "Crossref API (title/author/venue/year correct). Grey-listed "
                        "to avoid bibkey-based lookup false MISMATCH."),
}



def _config_for(mailto: str) -> _config.Config:
    return _config.Config(
        root=ROOT,
        mailto=mailto,
        citations={
            "grey_keys": sorted(NO_CROSSREF_EXPECTED),
            "watchlist": {key: note for key, note in WATCHLIST.values()},
        },
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tex", type=Path, default=DEFAULT_TEX,
                        help="manuscript holding the bibliography")
    parser.add_argument("--bib", help="standalone .bib file instead of a manuscript")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help="Markdown report path")
    parser.add_argument("--mailto", default=DEFAULT_MAILTO,
                        help="contact address for the Crossref polite pool")
    parser.add_argument("--rows", type=int, default=3, help="candidate rows per entry")
    parser.add_argument("--delay", type=float, default=1.0, help="seconds between queries")
    parser.add_argument("--timeout", type=float, default=20.0, help="per-request timeout")
    parser.add_argument("--no-network", action="store_true", help="parse only, query nothing")
    args = parser.parse_args(argv)

    return _cites.main(args, _config_for(args.mailto))


if __name__ == "__main__":
    sys.exit(main())

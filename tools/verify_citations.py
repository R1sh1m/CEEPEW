#!/usr/bin/env python3
"""Verify paper citations against Crossref.

Reads the ``thebibliography`` environment from
``docs/paper/ceepew_icitiit2027_overleaf_project/main_standalone_no_external_pdfs.tex``,
queries the Crossref REST API (polite pool) for DOI / title / author / venue,
and writes a Markdown mismatch report (default: ``tools/citation_report.md``).

Dependencies: stdlib + ``requests`` only.

Usage:
    python tools/verify_citations.py [--tex PATH] [--out PATH]
                                     [--mailto ADDR] [--no-network]
                                     [--rows N] [--delay SECS] [--timeout SECS]

Exit codes:
    0  report written successfully (mismatches do NOT fail the run;
       they are recorded in the report for a human to adjudicate).
    1  fatal error (unreadable .tex, network misconfiguration, write failure).
    2  parse failure (no ``thebibliography`` / zero ``\\bibitem`` entries found).

Notes:
    * Grey-literature entries (RFCs, GitHub repos/issues, NVD/CVE records,
      vendor user guides, benchmark reports, living specs such as the Noise
      framework) are NOT expected to resolve in Crossref. They are marked
      ``NO_CROSSREF_EXPECTED`` and listed for manual checking instead.
    * Watchlist entries flagged by the paper lane receive extra scrutiny in
      the report regardless of score: [19] ``atul2026``, [20] ``oryx_esp32``,
      [21] ``sarasa2025``, [32] ``vanderlinden2023``.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
import time
from pathlib import Path

try:
    import requests
except ImportError:  # pragma: no cover
    print("ERROR: the 'requests' package is required (pip install requests).",
          file=sys.stderr)
    sys.exit(1)

# ---------------------------------------------------------------------------
# Defaults (overridable via CLI)
# ---------------------------------------------------------------------------

_repo_root = Path(__file__).resolve().parent.parent
_candidate_tex = (_repo_root / "docs" / "paper" / "icitiit2027"
                  / "ceepew_icitiit2027_overleaf_project"
                  / "main_standalone_no_external_pdfs.tex")
if not _candidate_tex.exists():
    _candidate_tex = (_repo_root / "docs" / "paper"
                      / "ceepew_icitiit2027_overleaf_project"
                      / "main_standalone_no_external_pdfs.tex")
DEFAULT_TEX = _candidate_tex
DEFAULT_OUT = Path(__file__).resolve().parent / "citation_report.md"
CROSSREF_URL = "https://api.crossref.org/works"
USER_AGENT = "CEEPEW-citation-check/1.0 (mailto:{mailto})"

# Bibkeys that are grey literature / living specs / vendor docs: Crossref
# lookup is attempted opportunistically but a miss is NOT a mismatch.
NO_CROSSREF_EXPECTED = {
    "espressif_espnow",      # vendor user guide
    "espressif_issue15179",  # GitHub issue
    "nozomi2024",            # vendor blog/advisory
    "nvd_cve42483",          # NVD record
    "securepair2026",        # GitHub repo
    "painlessmesh",          # community mesh project page
    "oryx_esp32",            # vendor benchmark report (grey lit)
    "perrin2018",            # Noise spec (living document, no DOI)
    "rfc7748",               # IETF RFC (has DOI 10.17487/RFC7748, but
    "rfc5869",               #   checked via datatracker, not Crossref)
    "rfc9383",
    "cve2025_12888",         # NVD/CVE record
    "montiel2026",           # JAIIO proceedings (likely absent from Crossref)
}

# Entries the paper lane explicitly asked to scrutinise: {bib_number: note}.
WATCHLIST = {
    19: ("atul2026", "ePrint 2026/1882 — confirm paper exists under this ID "
                      "and author/title spelling matches the archive record."),
    20: ("oryx_esp32", "Grey-lit vendor benchmark (CycloneCRYPTO @ 240 MHz) — "
                        "no DOI expected; confirm URL/title/year manually."),
    21: ("sarasa2025", "Appl. Sci. 15(7):4071 — confirm article number, "
                        "author list, and that the Ascon-Arduino scope matches."),
    32: ("vanderlinden2023", "ESORICS 2023 Tamarin/Bluetooth paper — confirm "
                              "title, author spelling (Vanderlinden/Van Goethem/"
                              "Vanhoef), and venue against proceedings."),
}

SIM_MATCH = 0.85    # normalised title similarity >= this  -> MATCH
SIM_REVIEW = 0.55   # >= this -> REVIEW, below -> MISMATCH (when a candidate exists)


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

BIBITEM_RE = re.compile(r"\\bibitem\{(?P<key>[^}]+)\}\s*(?P<body>.*?)(?=\\bibitem|\\end\{thebibliography\})",
                        re.DOTALL)
TITLE_RE = re.compile(r"``(?P<title>.*?)''", re.DOTALL)
YEAR_RE = re.compile(r"\b(19|20)\d{2}\b")
ENV_RE = re.compile(r"\\begin\{thebibliography\}.*?\\end\{thebibliography\}", re.DOTALL)


def _clean_latex(s: str) -> str:
    """Strip common LaTeX accents/commands so titles compare cleanly."""
    s = re.sub(r"\\['\"`^~=.][{ ]?([A-Za-z])", r"\1", s)   # \'e -> e etc.
    s = re.sub(r"\\[a-zA-Z]+\s*", " ", s)                  # \textit etc.
    s = s.replace("{", " ").replace("}", " ")
    s = s.replace("~", " ").replace("\\", " ")
    s = re.sub(r"\s+", " ", s)
    return s.strip(" ,.;")


def _norm(s: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", s.lower()).strip()


def parse_bibliography(tex: str):
    """Return ordered list of dicts: number, key, raw, title, year."""
    m = ENV_RE.search(tex)
    if not m:
        return []
    entries = []
    for i, bm in enumerate(BIBITEM_RE.finditer(m.group(0)), start=1):
        key = bm.group("key").strip()
        body = bm.group("body").strip()
        raw_one_line = re.sub(r"\s+", " ", body)
        tm = TITLE_RE.search(body)
        title = _clean_latex(tm.group("title")) if tm else ""
        years = YEAR_RE.findall(raw_one_line)  # findall returns groups; re-scan below
        year_list = re.findall(r"\b((?:19|20)\d{2})\b", raw_one_line)
        year = year_list[-1] if year_list else ""
        entries.append({"number": i, "key": key, "raw": raw_one_line,
                        "title": title, "year": year})
    return entries


# ---------------------------------------------------------------------------
# Crossref
# ---------------------------------------------------------------------------

def crossref_lookup(session: requests.Session, title: str, mailto: str,
                    rows: int, timeout: float):
    """Query Crossref by bibliographic string; return (ok, payload|error)."""
    params = {"query.bibliographic": title, "rows": rows,
              "select": "DOI,title,author,container-title,publisher,published",
              "mailto": mailto}
    headers = {"User-Agent": USER_AGENT.format(mailto=mailto),
               "Accept": "application/json"}
    try:
        r = session.get(CROSSREF_URL, params=params, headers=headers,
                        timeout=timeout)
        if r.status_code == 429:
            retry = int(r.headers.get("Retry-After", "5"))
            time.sleep(min(retry, 30))
            r = session.get(CROSSREF_URL, params=params, headers=headers,
                            timeout=timeout)
        r.raise_for_status()
        return True, r.json()
    except Exception as exc:  # network, HTTP, JSON errors
        return False, str(exc)


def best_candidate(message: dict):
    items = (message or {}).get("items", [])
    if not items:
        return None
    top = items[0]
    titles = top.get("title") or [""]
    return {"DOI": top.get("DOI", ""),
            "title": titles[0] if titles else "",
            "authors": ["%s %s" % (a.get("given", ""), a.get("family", ""))
                        for a in (top.get("author") or [])][:6],
            "venue": (top.get("container-title") or [""])[0],
            "publisher": top.get("publisher", "")}


def score_title(local: str, remote: str) -> float:
    if not local or not remote:
        return 0.0
    return difflib.SequenceMatcher(None, _norm(local), _norm(remote)).ratio()


def verdict(entry: dict, cand: dict | None, sim: float) -> str:
    if entry["key"] in NO_CROSSREF_EXPECTED and cand is None:
        return "NO_CROSSREF_EXPECTED"
    if cand is None:
        return "NOT_FOUND"
    if sim >= SIM_MATCH:
        return "MATCH"
    if sim >= SIM_REVIEW:
        return "REVIEW"
    return "MISMATCH"


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def md_escape(s: str) -> str:
    return s.replace("|", "\\|").replace("\n", " ")


def build_report(entries, results, mailto: str) -> str:
    from datetime import date
    lines = []
    lines.append("# Citation verification report")
    lines.append("")
    lines.append("- Source: `docs/paper/ceepew_icitiit2027_overleaf_project/"
                 "main_standalone_no_external_pdfs.tex`")
    lines.append("- Entries parsed: %d" % len(entries))
    lines.append("- Backend: Crossref REST API (`%s`), polite pool "
                 "(mailto `%s`)" % (CROSSREF_URL, mailto if results else "n/a — offline"))
    lines.append("- Date: %s" % date.today().isoformat())
    lines.append("")
    lines.append("Verdict scale: `MATCH` (title similarity ≥ %.2f) · "
                 "`REVIEW` (≥ %.2f) · `MISMATCH` (< %.2f or wrong record) · "
                 "`NOT_FOUND` (no Crossref candidate) · "
                 "`NO_CROSSREF_EXPECTED` (grey literature / RFC / NVD / vendor doc — "
                 "manual check) · `ERROR` (lookup failed)." % (SIM_MATCH, SIM_REVIEW, SIM_REVIEW))
    lines.append("")
    lines.append("## Watchlist (paper-lane flags)")
    lines.append("")
    for num in sorted(WATCHLIST):
        key, note = WATCHLIST[num]
        v = results.get(key, {}).get("verdict", "n/a (offline or missing)")
        lines.append("- **[%d] `%s`** — verdict `%s`. %s" % (num, key, v, note))
    lines.append("")
    lines.append("## Summary table")
    lines.append("")
    lines.append("| # | key | year | verdict | sim | Crossref DOI / title |")
    lines.append("|---|-----|------|---------|-----|----------------------|")
    for e in entries:
        r = results.get(e["key"], {})
        doi = r.get("candidate", {}).get("DOI", "") if r.get("candidate") else r.get("error", "")
        ctitle = r.get("candidate", {}).get("title", "") if r.get("candidate") else ""
        cell = ("DOI %s — %s" % (doi, ctitle)).strip(" —") if (doi or ctitle) else "—"
        lines.append("| [%d] | `%s` | %s | `%s` | %s | %s |"
                     % (e["number"], e["key"], e["year"] or "—",
                        r.get("verdict", "SKIPPED"),
                        ("%.2f" % r["sim"]) if "sim" in r else "—",
                        md_escape(cell)))
    lines.append("")
    lines.append("## Per-entry detail")
    lines.append("")
    for e in entries:
        r = results.get(e["key"], {})
        lines.append("### [%d] `%s` — `%s`" % (e["number"], e["key"],
                                               r.get("verdict", "SKIPPED")))
        lines.append("")
        lines.append("- Local title: *%s*" % md_escape(e["title"] or "(no ``quoted'' title found)"))
        lines.append("- Local raw: `%s`" % md_escape(e["raw"][:400]))
        cand = r.get("candidate")
        if cand:
            lines.append("- Crossref title: *%s*" % md_escape(cand.get("title", "")))
            lines.append("- DOI: %s" % (cand.get("DOI", "") or "—"))
            lines.append("- Authors: %s" % md_escape("; ".join(cand.get("authors", [])) or "—"))
            lines.append("- Venue: *%s*" % md_escape(cand.get("venue", "") or "—"))
            lines.append("- Title similarity: %.3f" % r.get("sim", 0.0))
        elif r.get("error"):
            lines.append("- Lookup error: `%s`" % md_escape(r["error"][:300]))
        else:
            lines.append("- Crossref candidate: none returned.")
        if e["number"] in WATCHLIST:
            lines.append("- Watchlist note: %s" % WATCHLIST[e["number"]][1])
        lines.append("")
    lines.append("## Follow-up checklist")
    lines.append("")
    lines.append("- [ ] Adjudicate every `MISMATCH` / `REVIEW` row above against "
                 "the publisher page (DOI link) — fix the `.tex` entry or record "
                 "why the local citation is intentionally different.")
    lines.append("- [ ] Manually confirm each `NO_CROSSREF_EXPECTED` entry "
                 "(open the RFC / NVD / GitHub / vendor URL, check title + year).")
    lines.append("- [ ] Re-run this script after any `.tex` bibliography edit: "
                 "`python tools/verify_citations.py`.")
    lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Verify paper bibliography against Crossref.")
    ap.add_argument("--tex", type=Path, default=DEFAULT_TEX,
                    help="Path to main_standalone_no_external_pdfs.tex")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT,
                    help="Markdown report output path")
    ap.add_argument("--mailto", default="ceepew-paper@example.com",
                    help="Contact email sent to Crossref polite pool")
    ap.add_argument("--rows", type=int, default=3,
                    help="Candidates requested per entry")
    ap.add_argument("--delay", type=float, default=1.0,
                    help="Seconds to sleep between Crossref queries")
    ap.add_argument("--timeout", type=float, default=20.0,
                    help="Per-request HTTP timeout (seconds)")
    ap.add_argument("--no-network", action="store_true",
                    help="Parse only; skip Crossref queries (offline smoke test)")
    args = ap.parse_args(argv)

    try:
        tex = args.tex.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print("ERROR: cannot read .tex file %s: %s" % (args.tex, exc), file=sys.stderr)
        return 1

    entries = parse_bibliography(tex)
    if not entries:
        print("ERROR: no thebibliography/\\bibitem entries parsed from %s" % args.tex,
              file=sys.stderr)
        return 2
    print("Parsed %d bibliography entries from %s" % (len(entries), args.tex))

    results: dict = {}
    if args.no_network:
        for e in entries:
            results[e["key"]] = {"verdict": "SKIPPED",
                                 "error": "offline mode (--no-network)"}
    else:
        session = requests.Session()
        for i, e in enumerate(entries):
            if not e["title"]:
                results[e["key"]] = {"verdict": "REVIEW",
                                     "error": "no quoted title to query"}
                continue
            ok, payload = crossref_lookup(session, e["title"], args.mailto,
                                          args.rows, args.timeout)
            if not ok:
                results[e["key"]] = {"verdict": "ERROR", "error": payload}
            else:
                try:
                    cand = best_candidate(payload.get("message", {}))
                except Exception as exc:
                    results[e["key"]] = {"verdict": "ERROR",
                                         "error": "bad Crossref payload: %s" % exc}
                    cand = None
                    sim = 0.0
                else:
                    sim = score_title(e["title"], cand["title"]) if cand else 0.0
                    results[e["key"]] = {"verdict": verdict(e, cand, sim),
                                         "candidate": cand, "sim": sim}
            v = results[e["key"]]["verdict"]
            print("[%2d/%d] [%d] %-20s -> %s"
                  % (i + 1, len(entries), e["number"], e["key"], v))
            if i + 1 < len(entries):
                time.sleep(args.delay)

    report = build_report(entries, results, args.mailto)
    try:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(report, encoding="utf-8")
    except OSError as exc:
        print("ERROR: cannot write report %s: %s" % (args.out, exc), file=sys.stderr)
        return 1

    counts: dict[str, int] = {}
    for r in results.values():
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print("Wrote %s  verdicts=%s" % (args.out, counts))
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Generate booktabs LaTeX tables from ``results/registry.json``.

Usage::

    python tools/journal/registry_tables.py --out DIR [--registry PATH]

The registry is read-only here.  One LaTeX fragment is written per evidence
class (``M-HW``, ``M-HOST``, ``C``, ``S``, ``E``) plus a combined
``registry_all.tex`` that carries an extra Class column.  Rows are never
synthesised: every row comes from one registry entry, and an absent field is
rendered as an em dash (``---``).

Required LaTeX packages for the emitted fragments: ``booktabs``, ``longtable``,
``array``.
"""

from __future__ import annotations

import argparse
import json
import os
import re
from typing import Any, Iterable, Mapping, Sequence

# Evidence classes, in the canonical order used throughout the paper.
CLASS_ORDER: tuple[str, ...] = ("M-HW", "M-HOST", "C", "S", "E")

DESCRIPTION_MAX_CHARS = 70
VALUE_MAX_CHARS = 60
UNIT_MAX_CHARS = 24
SOURCE_MAX_CHARS = 58
ID_MAX_CHARS = 34

EM_DASH = "---"

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(os.path.dirname(_HERE))
DEFAULT_REGISTRY = os.path.join(_REPO_ROOT, "results", "registry.json")

# Single-pass character translation: no replacement can re-trigger another.
_ESCAPE_MAP = {
    ord("\\"): r"\textbackslash{}",
    ord("&"): r"\&",
    ord("%"): r"\%",
    ord("$"): r"\$",
    ord("#"): r"\#",
    ord("_"): r"\_",
    ord("{"): r"\{",
    ord("}"): r"\}",
    ord("~"): r"\textasciitilde{}",
    ord("^"): r"\textasciicircum{}",
}

# Unicode punctuation is transliterated to ASCII *before* escaping so that the
# fragments do not depend on the includer's input encoding.
_UNICODE_MAP = {
    "\u2014": "---",  # em dash
    "\u2013": "--",   # en dash
    "\u2212": "-",    # minus sign
    "\u2018": "'",
    "\u2019": "'",
    "\u201c": "''",
    "\u201d": "''",
    "\u2026": "...",
    "\u00d7": "x",
    "\u00b7": ".",
    "\u2192": "->",
    "\u2190": "<-",
    "\u2264": "<=",
    "\u2265": ">=",
    "\u2248": "~=",
    "\u00b1": "+/-",
    "\u00b5": "u",
    "\u03bc": "u",
    "\u00b0": "deg",
    "\u00a0": " ",
    "\u2009": " ",
    "\u202f": " ",
}

# Source-dict keys that do not name a data artefact, in preference order they
# are skipped when looking for the "source file" column.
_SOURCE_PREFERENCE: tuple[str, ...] = (
    "script",
    "raw_log",
    "derived",
    "manifest",
    "analysis",
    "site",
    "reference",
    "run_notes",
)
_SOURCE_SKIP: frozenset[str] = frozenset(
    {
        "note",
        "status",
        "locators",
        "binary_sha256",
        "receiver_sha256",
        "sender_sha256",
        "runs",
        "replicate",
        "per_device_medians_us",
        "device_scope",
        "tag_resolution",
        "cross_check",
        "reference_sha256",
    }
)


# ---------------------------------------------------------------------------
# text rendering
# ---------------------------------------------------------------------------
def latex_escape(text: str) -> str:
    """Escape LaTeX-special characters: ``& % _ # $ { } ~ ^ \\``.

    Applied as a single ``str.translate`` pass so that the braces introduced by
    ``\\textbackslash{}`` are not escaped again.  Non-ASCII characters are
    transliterated where a sensible ASCII form exists and replaced with ``?``
    otherwise.
    """
    if not isinstance(text, str):
        raise ValueError(f"latex_escape expects str, got {type(text).__name__}")
    normalised = "".join(_UNICODE_MAP.get(ch, ch) for ch in text)
    ascii_only = "".join(ch if ord(ch) < 128 else "?" for ch in normalised)
    return ascii_only.translate(_ESCAPE_MAP)


def _truncate(text: str, limit: int) -> str:
    """Truncate ``text`` to ``limit`` characters, marking elision with ``...``.

    Whitespace is collapsed first so a cell never carries newlines or doubled
    spaces.  The cut is pulled back to a word boundary when doing so retains at
    least 60 percent of the budget, which keeps generated cells readable.
    """
    if limit <= 3:
        raise ValueError(f"limit must exceed 3, got {limit}")
    stripped = " ".join(text.split())
    if len(stripped) <= limit:
        return stripped
    cut = stripped[: limit - 3].rstrip()
    if cut and stripped[len(cut)] != " ":
        space = cut.rfind(" ")
        if space >= int(limit * 0.6):
            cut = cut[:space].rstrip()
    if not cut:
        cut = stripped[: limit - 3].rstrip()
    return cut + "..."


def _cell(raw: Any, limit: int) -> str:
    """Render one registry field as a truncated, escaped table cell."""
    if raw is None:
        return EM_DASH
    text = str(raw).strip()
    if not text:
        return EM_DASH
    return latex_escape(_truncate(text, limit))


def _flatten_source_value(value: Any) -> str:
    """Render a single ``source`` entry (string or list of strings) as text."""
    if isinstance(value, (list, tuple)):
        parts = [str(v).strip() for v in value if str(v).strip()]
        return "; ".join(parts)
    return str(value).strip()


def _source_label(source: Any) -> str:
    """Pick the most informative provenance string out of a registry ``source``.

    The registry stores ``source`` as a free-form object whose keys vary per
    entry (``script``, ``raw_log``, ``derived``, ``analysis``, ``site``,
    ``manifest``, ...).  Preference order is :data:`_SOURCE_PREFERENCE`, then
    the first remaining non-skipped key.  Absent provenance renders as an em
    dash; nothing is inferred.
    """
    if source is None:
        return EM_DASH
    if isinstance(source, str):
        return _cell(source, SOURCE_MAX_CHARS)
    if not isinstance(source, Mapping):
        return _cell(source, SOURCE_MAX_CHARS)

    for key in _SOURCE_PREFERENCE:
        if key in source:
            text = _flatten_source_value(source[key])
            if text:
                return _cell(text, SOURCE_MAX_CHARS)
    for key, value in source.items():
        if key in _SOURCE_SKIP:
            continue
        text = _flatten_source_value(value)
        if text:
            return _cell(text, SOURCE_MAX_CHARS)
    return EM_DASH


def _class_slug(class_name: str) -> str:
    """Filesystem/label-safe slug for an evidence class (``M-HW`` -> ``m_hw``)."""
    slug = re.sub(r"[^a-z0-9]+", "_", class_name.lower()).strip("_")
    if not slug:
        raise ValueError(f"cannot derive a slug for class {class_name!r}")
    return slug


# ---------------------------------------------------------------------------
# registry loading and grouping
# ---------------------------------------------------------------------------
def load_registry(path: str) -> dict:
    """Load and minimally validate the registry document."""
    with open(path, "r", encoding="utf-8") as handle:
        document = json.load(handle)
    if not isinstance(document, dict):
        raise ValueError(f"{path}: top level must be a JSON object")
    entries = document.get("entries")
    if not isinstance(entries, list):
        raise ValueError(f"{path}: missing 'entries' list")
    seen: set[str] = set()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise ValueError(f"{path}: entries[{index}] is not an object")
        entry_id = entry.get("id")
        if not isinstance(entry_id, str) or not entry_id.strip():
            raise ValueError(f"{path}: entries[{index}] has no usable 'id'")
        if entry_id in seen:
            raise ValueError(f"{path}: duplicate entry id {entry_id!r}")
        seen.add(entry_id)
        class_name = entry.get("class")
        if class_name not in CLASS_ORDER:
            raise ValueError(
                f"{path}: entry {entry_id!r} has class {class_name!r}; expected one of {CLASS_ORDER}"
            )
    return document


def group_by_class(entries: Iterable[Mapping[str, Any]]) -> dict[str, list[Mapping[str, Any]]]:
    """Group entries by evidence class, each group sorted by ``id``."""
    grouped: dict[str, list[Mapping[str, Any]]] = {name: [] for name in CLASS_ORDER}
    for entry in entries:
        grouped[str(entry["class"])].append(entry)
    for name in CLASS_ORDER:
        grouped[name].sort(key=lambda e: str(e["id"]))
    return grouped


def sort_entries(entries: Iterable[Mapping[str, Any]]) -> list[Mapping[str, Any]]:
    """Deterministic total order: evidence-class rank, then ``id``."""
    return sorted(entries, key=lambda e: (CLASS_ORDER.index(str(e["class"])), str(e["id"])))


# ---------------------------------------------------------------------------
# LaTeX emission
# ---------------------------------------------------------------------------
def _row(entry: Mapping[str, Any], include_class: bool) -> str:
    """Emit one booktabs row from one registry entry."""
    cells = []
    if include_class:
        cells.append(latex_escape(str(entry["class"])))
    cells.append(_cell(entry.get("id"), ID_MAX_CHARS))
    cells.append(_cell(entry.get("description"), DESCRIPTION_MAX_CHARS))
    cells.append(_cell(entry.get("value"), VALUE_MAX_CHARS))
    cells.append(_cell(entry.get("unit"), UNIT_MAX_CHARS))
    cells.append(_cell(entry.get("n"), 12))
    cells.append(_source_label(entry.get("source")))
    return " & ".join(cells) + r" \\"


_HEADER_COLUMNS = ("ID", "Description", "Value", "Unit", "$n$", "Source")

_COLUMN_SPEC = (
    r"{@{}>{\raggedright\arraybackslash}p{0.115\linewidth}"
    r" >{\raggedright\arraybackslash}p{0.275\linewidth}"
    r" >{\raggedright\arraybackslash}p{0.150\linewidth}"
    r" l"
    r" r"
    r" >{\raggedright\arraybackslash}p{0.175\linewidth}@{}}"
)
_COLUMN_COUNT = 6

_COMBINED_COLUMN_SPEC = (
    r"{@{}l"
    r" >{\raggedright\arraybackslash}p{0.105\linewidth}"
    r" >{\raggedright\arraybackslash}p{0.235\linewidth}"
    r" >{\raggedright\arraybackslash}p{0.130\linewidth}"
    r" l"
    r" r"
    r" >{\raggedright\arraybackslash}p{0.150\linewidth}@{}}"
)
_COMBINED_COLUMN_COUNT = 7


def _header_cells(include_class: bool) -> str:
    names = (("Class",) + _HEADER_COLUMNS) if include_class else _HEADER_COLUMNS
    return " & ".join(names) + r" \\"


def _provenance_comment(document: Mapping[str, Any], source_path: str) -> list[str]:
    """Header comments recording exactly which registry produced the table."""
    rel = os.path.relpath(source_path, _REPO_ROOT).replace(os.sep, "/")
    return [
        "% Generated by tools/journal/registry_tables.py -- do not edit by hand.",
        f"% Source registry: {rel}",
        f"% schema_version={document.get('schema_version', '?')} "
        f"updated_utc={document.get('updated_utc', '?')}",
        "% Requires LaTeX packages: booktabs, longtable, array.",
        "% Descriptions are truncated at "
        f"{DESCRIPTION_MAX_CHARS} chars, values at {VALUE_MAX_CHARS}; "
        "'---' means the field is absent in the registry.",
    ]


def build_class_table(
    entries: Sequence[Mapping[str, Any]],
    class_name: str,
    document: Mapping[str, Any],
    source_path: str,
) -> str:
    """Return a complete ``longtable`` fragment for one evidence class."""
    slug = _class_slug(class_name)
    caption = (
        f"CEE-PEW results registry: evidence class \\texttt{{{latex_escape(class_name)}}} "
        f"({len(entries)} entries)."
    )
    lines = _provenance_comment(document, source_path)
    lines.append(f"% Evidence class {class_name}: {len(entries)} entries")
    lines.append(r"\begin{longtable}" + _COLUMN_SPEC)
    lines.append(rf"\caption{{{caption}}}\label{{tab:registry_{slug}}}\\")
    lines.append(r"\toprule")
    lines.append(_header_cells(include_class=False))
    lines.append(r"\midrule")
    lines.append(r"\endfirsthead")
    lines.append(
        rf"\multicolumn{{{_COLUMN_COUNT}}}{{@{{}}l}}{{\small\emph{{Table \ref{{tab:registry_{slug}}} (continued)}}}}\\"
    )
    lines.append(r"\toprule")
    lines.append(_header_cells(include_class=False))
    lines.append(r"\midrule")
    lines.append(r"\endhead")
    lines.append(r"\bottomrule")
    lines.append(r"\endfoot")
    if entries:
        lines.extend(_row(entry, include_class=False) for entry in entries)
    else:
        lines.append(
            rf"\multicolumn{{{_COLUMN_COUNT}}}{{@{{}}l}}"
            r"{\emph{no entries of this evidence class in the registry}}\\"
        )
    lines.append(r"\end{longtable}")
    return "\n".join(lines) + "\n"


def build_combined_table(
    entries: Sequence[Mapping[str, Any]],
    document: Mapping[str, Any],
    source_path: str,
    counts: Mapping[str, int],
) -> str:
    """Return the combined ``longtable`` fragment covering every class."""
    total = sum(counts.values())
    summary = ", ".join(f"{name}={counts[name]}" for name in CLASS_ORDER)
    caption = (
        f"CEE-PEW results registry: all {total} entries by evidence class "
        f"({latex_escape(summary)})."
    )
    lines = _provenance_comment(document, source_path)
    lines.append(f"% Entry counts by class: {summary} (total {total})")
    for entry in entries:
        if "superseded_by" in entry:
            lines.append(
                f"% NOTE: entry {entry['id']} is marked superseded_by="
                f"{entry['superseded_by']} in the registry; it is listed unchanged."
            )
    lines.append(r"\begin{longtable}" + _COMBINED_COLUMN_SPEC)
    lines.append(rf"\caption{{{caption}}}\label{{tab:registry_all}}\\")
    lines.append(r"\toprule")
    lines.append(_header_cells(include_class=True))
    lines.append(r"\midrule")
    lines.append(r"\endfirsthead")
    lines.append(
        rf"\multicolumn{{{_COMBINED_COLUMN_COUNT}}}{{@{{}}l}}"
        r"{\small\emph{Table \ref{tab:registry_all} (continued)}}\\"
    )
    lines.append(r"\toprule")
    lines.append(_header_cells(include_class=True))
    lines.append(r"\midrule")
    lines.append(r"\endhead")
    lines.append(r"\bottomrule")
    lines.append(r"\endfoot")
    if entries:
        lines.extend(_row(entry, include_class=True) for entry in entries)
    else:
        lines.append(
            rf"\multicolumn{{{_COMBINED_COLUMN_COUNT}}}{{@{{}}l}}"
            r"{\emph{registry contains no entries}}\\"
        )
    lines.append(r"\end{longtable}")
    return "\n".join(lines) + "\n"


def write_tables(
    out_dir: str,
    registry_path: str = DEFAULT_REGISTRY,
) -> dict[str, Any]:
    """Write one fragment per class plus the combined fragment.

    Returns a summary dict: ``{"counts": {class: n}, "total": n, "files": [...]}``.
    Creates ``out_dir`` if needed; raises :class:`ValueError` on a malformed
    registry.
    """
    document = load_registry(registry_path)
    entries = sort_entries(document["entries"])
    grouped = group_by_class(entries)
    os.makedirs(out_dir, exist_ok=True)

    counts = {name: len(grouped[name]) for name in CLASS_ORDER}
    files: list[str] = []
    for class_name in CLASS_ORDER:
        path = os.path.join(out_dir, f"registry_{_class_slug(class_name)}.tex")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(build_class_table(grouped[class_name], class_name, document, registry_path))
        files.append(path)

    combined_path = os.path.join(out_dir, "registry_all.tex")
    with open(combined_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(build_combined_table(entries, document, registry_path, counts))
    files.append(combined_path)

    return {"counts": counts, "total": len(entries), "files": files}


def main(argv: Sequence[str] | None = None) -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Emit booktabs LaTeX tables from results/registry.json (read-only)."
    )
    parser.add_argument("--out", required=True, help="output directory for .tex fragments")
    parser.add_argument(
        "--registry",
        default=DEFAULT_REGISTRY,
        help=f"registry JSON path (default: {os.path.relpath(DEFAULT_REGISTRY, _REPO_ROOT)})",
    )
    args = parser.parse_args(argv)

    if not os.path.isfile(args.registry):
        raise SystemExit(f"registry not found: {args.registry}")

    summary = write_tables(args.out, args.registry)
    counts = summary["counts"]
    print(f"registry: {os.path.relpath(args.registry, _REPO_ROOT)}")
    print(f"out dir : {args.out}")
    for name in CLASS_ORDER:
        print(f"  {name:<7} {counts[name]:>3} entries -> registry_{_class_slug(name)}.tex")
    print(f"  {'ALL':<7} {summary['total']:>3} entries -> registry_all.tex")
    for path in summary["files"]:
        size = os.path.getsize(path)
        print(f"wrote {path} ({size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

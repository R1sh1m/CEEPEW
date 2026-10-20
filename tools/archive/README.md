# tools/archive/ — Archived Historical & One-Off Scripts

This directory houses historical, single-purpose, or completed experiment scripts from earlier development and capture milestones. They are retained for research provenance and audit trail purposes, but are not part of the active, reusable testing pipeline.

## Archived Contents

| File | Original Location | Purpose / History |
|------|-------------------|-------------------|
| `build_technical_report.py` | `tools/` | One-off Python script (PyMuPDF + python-docx) used to compile the 937-line illustrated CEE-PEW technical report from presentation SVGs. |
| `_do_build_ccmp.ps1` | `tools/` | Ad-hoc background build runner for `build_cap_ccmp`. |
| `_do_build_ceepew.ps1` | `tools/` | Ad-hoc background build runner for `build_cap_ceepew`. |
| `monitor_dual.py` | `tools/` | Barebones dual-port serial reader script; superseded by `tools/ceepew_monitor.py`. |
| `capture/rebuild_bench.ps1` | `tools/capture/` | Sequential rebuild script iteration 1 for capture bench firmwares. |
| `capture/rebuild_bench2.ps1` | `tools/capture/` | Sequential rebuild script iteration 2 for capture bench firmwares. |
| `capture/ota_bench.ps1` | `tools/capture/` | Ad-hoc OTA benchmark flashing and capture invocation helper. |

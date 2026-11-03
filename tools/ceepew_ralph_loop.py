#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CEE-PEW RALPH Loop Harness (Rapid Autonomous Loop for Patching & Hardening)
=============================================================================
Provides a unified multi-stage verification runner for the OpenCode Reviewer agent
and human engineers. Designed for tight iterative feedback loops:
  1. Audit/Test -> 2. Parse Failures -> 3. Patch -> 4. Re-Verify -> 5. Converge

Usage:
  python tools/ceepew_ralph_loop.py --mode paper     # Audit & verify monograph LaTeX, arithmetic, lint, citations
  python tools/ceepew_ralph_loop.py --mode host      # Run C host unit tests (ASan/UBSan, crypto, FEC, zeroing)
  python tools/ceepew_ralph_loop.py --mode gate      # Execute full pre-publication gate
  python tools/ceepew_ralph_loop.py --mode all       # Run complete end-to-end Ralph Loop verification
  python tools/ceepew_ralph_loop.py --json           # Output structured JSON for agent automated parsing
"""

import argparse
import json
import os
import subprocess
import sys
import time

try:
    import certifi
    if "SSL_CERT_FILE" not in os.environ:
        os.environ["SSL_CERT_FILE"] = certifi.where()
except Exception:
    pass

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PAPER_FINAL_TEX = os.path.join(ROOT, "docs", "paper", "The Paper", "FINAL", "ceepew_unrestricted.tex")
PAPER_ROOT_TEX = os.path.join(ROOT, "docs", "paper", "The Paper", "main.tex")

class RalphStage:
    def __init__(self, name, command, cwd=ROOT, critical=True):
        self.name = name
        self.command = command
        self.cwd = cwd
        self.critical = critical

    def run(self):
        t0 = time.time()
        try:
            res = subprocess.run(
                self.command,
                cwd=self.cwd,
                capture_output=True,
                text=True,
                errors="replace",
                shell=isinstance(self.command, str)
            )
            elapsed = time.time() - t0
            passed = (res.returncode == 0)
            return {
                "name": self.name,
                "passed": passed,
                "returncode": res.returncode,
                "elapsed_sec": round(elapsed, 2),
                "stdout": res.stdout.strip(),
                "stderr": res.stderr.strip()
            }
        except Exception as e:
            elapsed = time.time() - t0
            return {
                "name": self.name,
                "passed": False,
                "returncode": -1,
                "elapsed_sec": round(elapsed, 2),
                "stdout": "",
                "stderr": str(e)
            }

def run_ralph_loop(mode="paper", fast=False, output_json=False):
    stages = []

    if mode in ("paper", "all"):
        stages.append(RalphStage(
            "Paper Lint (Banned Tokens & Em Dashes)",
            [sys.executable, os.path.join(ROOT, "tools", "paper_lint.py"), PAPER_FINAL_TEX]
        ))
        stages.append(RalphStage(
            "Arithmetic & Number Consistency",
            [sys.executable, os.path.join(ROOT, "tools", "recompute_paper_numbers.py")]
        ))
        cit_cmd = [sys.executable, os.path.join(ROOT, "tools", "verify_citations.py")]
        if fast:
            cit_cmd.append("--no-network")
        stages.append(RalphStage(
            "Citation & DOI Crossref Audit",
            cit_cmd
        ))
        stages.append(RalphStage(
            "Originality & Overlap Audit",
            [sys.executable, os.path.join(ROOT, "tools", "originality_audit.py"), "--quiet", PAPER_FINAL_TEX]
        ))
        stages.append(RalphStage(
            "Humanizer De-AI Prose Audit (26 Wikipedia Patterns)",
            [sys.executable, os.path.join(ROOT, "tools", "humanizer_lint.py"), "--quiet", PAPER_FINAL_TEX]
        ))
        stages.append(RalphStage(
            "Documentation & Repo Synchronization",
            [sys.executable, os.path.join(ROOT, "tools", "verify_doc_sync.py")]
        ))

    if mode in ("host", "all"):
        stages.append(RalphStage(
            "Host C Unit Tests (ASan/UBSan, 18/18)",
            [sys.executable, os.path.join(ROOT, "tools", "ceepew_pipeline.py"), "--mode", "host"]
        ))

    if mode in ("gate", "all"):
        gate_cmd = [sys.executable, os.path.join(ROOT, "tools", "publication_gate.py"), "--paper", "the_paper"]
        if fast:
            gate_cmd.append("--offline")
        else:
            gate_cmd.append("--external")
        stages.append(RalphStage(
            "Pre-Publication Gate (the_paper)",
            gate_cmd
        ))

    results = []
    all_passed = True

    if not output_json:
        print("=" * 72)
        print(" [RALPH LOOP] CEE-PEW Rapid Iteration & Verification Engine")
        print(f" Target Mode: {mode.upper()} | Working Dir: {ROOT}")
        print("=" * 72)

    for st in stages:
        if not output_json:
            print(f"--> Executing: {st.name} ... ", end="", flush=True)
        res = st.run()
        results.append(res)
        status = "PASS" if res["passed"] else "FAIL"
        if not res["passed"]:
            all_passed = False
        if not output_json:
            print(f"[{status}] ({res['elapsed_sec']}s)")
            if not res["passed"]:
                print("\n" + "-" * 50)
                print(f" [!] ERROR IN {st.name}:")
                if res["stderr"]:
                    print(res["stderr"][:1500])
                if res["stdout"]:
                    print(res["stdout"][-1500:])
                print("-" * 50 + "\n")

    summary = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "mode": mode,
        "all_passed": all_passed,
        "stages": results
    }

    if output_json:
        print(json.dumps(summary, indent=2))
    else:
        print("=" * 72)
        total_time = sum(r["elapsed_sec"] for r in results)
        passed_count = sum(1 for r in results if r["passed"])
        print(f" Verdict: {'CONVERGED (ALL PASSED)' if all_passed else 'REPAIRS NEEDED (FAILURES DETECTED)'}")
        print(f" Score: {passed_count}/{len(results)} stages passed ({total_time:.2f}s total)")
        print("=" * 72)

    return 0 if all_passed else 1

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="CEE-PEW Autonomous Ralph Loop Harness")
    parser.add_argument("--mode", choices=["paper", "host", "gate", "all"], default="paper",
                        help="Execution mode (paper, host, gate, all)")
    parser.add_argument("--fast", "--offline", action="store_true", help="Run local checks without slow network polling")
    parser.add_argument("--json", action="store_true", help="Output results as machine-readable JSON")
    args = parser.parse_args()

    sys.exit(run_ralph_loop(mode=args.mode, fast=args.fast, output_json=args.json))

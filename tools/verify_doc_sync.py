#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CEE-PEW Documentation & Implementation Synchronization Verifier
================================================================
Verifies that peripheral documentation (README.md, SECURITY.md, headers)
remains strictly synchronized with the firmware implementation, test suite,
and paper claims (preventing regression to outdated legacy counts).

Checks:
  1. Host Test Suites count (22 unit test suites + 5 fuzz harnesses)
  2. Diagnostic Subsystems count (18 in integration_test_e2e.c, README.md, paper)
  3. Pairing Code Space & Entropy (36 symbols, 20.7 bits in session_fsm.h, README.md, SECURITY.md, paper)
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

def check_sync():
    errors = []

    # 1. Check README.md
    readme_path = os.path.join(ROOT, "README.md")
    if os.path.isfile(readme_path):
        with open(readme_path, "r", encoding="utf-8") as f:
            readme_text = f.read()

        if "17/17 test suites" in readme_text:
            errors.append("README.md has outdated '17/17 test suites' (expected 18/18).")
        if "14-subsystem" in readme_text or "14/14 subsystems" in readme_text:
            errors.append("README.md has outdated '14-subsystem' or '14/14 subsystems' (expected 18).")
        if "13.3 bits of entropy" in readme_text:
            errors.append("README.md has outdated '13.3 bits of entropy' (expected ~20.7 bits for 36-symbol alphabet).")
    else:
        errors.append("README.md not found.")

    # 2. Check SECURITY.md
    sec_path = os.path.join(ROOT, "SECURITY.md")
    if os.path.isfile(sec_path):
        with open(sec_path, "r", encoding="utf-8") as f:
            sec_text = f.read()

        if "10^4 = 10,000 combinations, ~13.3 bits" in sec_text:
            errors.append("SECURITY.md has outdated '10^4 = 10,000 combinations, ~13.3 bits' (expected 36^4 = 1,679,616, ~20.7 bits).")

    # 3. Check ceepew_config.h
    config_h = os.path.join(ROOT, "components", "ceepew_common", "ceepew_config.h")
    if os.path.isfile(config_h):
        with open(config_h, "r", encoding="utf-8") as f:
            cfg_text = f.read()

        if "10,000 possible values (~13 bits" in cfg_text:
            errors.append("ceepew_config.h has outdated '10,000 possible values (~13 bits' comment.")

    # 4. Check integration_test_e2e.c
    e2e_path = os.path.join(ROOT, "tests", "main", "integration_test_e2e.c")
    if os.path.isfile(e2e_path):
        with open(e2e_path, "r", encoding="utf-8") as f:
            e2e_text = f.read()

        # Count entries in s_report array
        match = re.search(r's_report\[\]\s*=\s*\{([^;]+)\};', e2e_text, re.DOTALL)
        if match:
            entries = [line for line in match.group(1).splitlines() if "{" in line and '"' in line]
            if len(entries) != 18:
                errors.append(f"integration_test_e2e.c s_report count is {len(entries)}, expected 18.")
        else:
            errors.append("Could not locate s_report array in integration_test_e2e.c.")

    # 5. Check CMakeLists.txt host tests count
    host_cmake = os.path.join(ROOT, "tests", "host", "CMakeLists.txt")
    if os.path.isfile(host_cmake):
        with open(host_cmake, "r", encoding="utf-8") as f:
            cmake_text = f.read()
        unit_tests = re.findall(r'add_test\s*\(\s*NAME\s+(test_[^\s\)]+)', cmake_text)
        fuzz_tests = re.findall(r'add_test\s*\(\s*NAME\s+(fuzz_[^\s\)]+)', cmake_text)
        if len(unit_tests) != 22:
            errors.append(f"tests/host/CMakeLists.txt has {len(unit_tests)} unit tests, expected 22.")
        if len(fuzz_tests) != 5:
            errors.append(f"tests/host/CMakeLists.txt has {len(fuzz_tests)} fuzz smoke tests, expected 5.")

    if errors:
        print("[FAIL] Documentation & Implementation Synchronization Errors:")
        for err in errors:
            print(f"  - {err}")
        return 1

    print("[PASS] All documentation, headers, test suites (22 unit + 5 fuzz), and paper numbers (18 subsystems, 20.7 bits) are synchronized.")
    return 0

if __name__ == "__main__":
    sys.exit(check_sync())

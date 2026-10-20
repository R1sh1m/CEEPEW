#!/usr/bin/env python3
"""Prepare sdkconfig for build_cap_timing with E5 timing instrumentation.

NOTE: This is a legacy single-config patcher. For general benchmark
configuration preparation, see tools/bench_prepare_config.py.
"""
import re
import os
import sys

sdkconfig_path = r"build_cap_timing\sdkconfig"
if not os.path.exists(sdkconfig_path):
    print(f"[FAIL] no sdkconfig at {sdkconfig_path}")
    sys.exit(1)

with open(sdkconfig_path, 'r', encoding='utf-8') as f:
    text = f.read()

# Enable CEEPEW_BENCH_TIMING
text = re.sub(r'^# CONFIG_CEEPEW_BENCH_TIMING is not set$', 'CONFIG_CEEPEW_BENCH_TIMING=y', text, flags=re.M)
text = re.sub(r'^CONFIG_CEEPEW_BENCH_TIMING=n$', 'CONFIG_CEEPEW_BENCH_TIMING=y', text, flags=re.M)

# Set CEEPEW_PIN_SYNC to 14
text = re.sub(r'^CONFIG_CEEPEW_PIN_SYNC=\d+$', 'CONFIG_CEEPEW_PIN_SYNC=14', text, flags=re.M)
if 'CONFIG_CEEPEW_PIN_SYNC' not in text:
    text += '\nCONFIG_CEEPEW_PIN_SYNC=14\n'

with open(sdkconfig_path, 'w', encoding='utf-8', newline='\n') as f:
    f.write(text)

print("Updated sdkconfig for CEEPEW_BENCH_TIMING")

# Verify
with open(sdkconfig_path, 'r', encoding='utf-8') as f:
    for line in f:
        if 'CEEPEW_BENCH_TIMING' in line or 'CEEPEW_PIN_SYNC' in line:
            print(f"  {line.rstrip()}")
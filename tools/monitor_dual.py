#!/usr/bin/env python3
"""tools/monitor_dual.py - [DEPRECATED] Backward-compatibility forwarder.

This script was a one-off dual-port reader. Canonical dual-port monitoring
is handled by tools/ceepew_monitor.py:
    python tools/ceepew_monitor.py --port COM3 --port COM4 --log session.txt

This wrapper delegates to tools/ceepew_monitor.py or tools/archive/monitor_dual.py.
"""
import sys
import os
import subprocess

print("[NOTICE] tools/monitor_dual.py is deprecated. Use tools/ceepew_monitor.py instead.", file=sys.stderr)

ARCHIVE_SCRIPT = os.path.join(os.path.dirname(__file__), "archive", "monitor_dual.py")
if os.path.exists(ARCHIVE_SCRIPT):
    cmd = [sys.executable, ARCHIVE_SCRIPT] + sys.argv[1:]
    sys.exit(subprocess.call(cmd))
else:
    print("[ERROR] Archived monitor_dual.py not found.", file=sys.stderr)
    sys.exit(1)

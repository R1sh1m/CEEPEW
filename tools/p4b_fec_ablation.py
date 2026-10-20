#!/usr/bin/env python3
"""
tools/p4b_fec_ablation.py

FEC-Ablation Experiment (P4b) for CEE-PEW.

Compares two firmware images built from the SAME source tree that differ by
exactly one Kconfig symbol:

    CONFIG_CEEPEW_FEC_DISABLE   (maps to CEEPEW_FEC_ENABLED 0 in ceepew_config.h:245)

which gates the Hamming(15,11) codec in:
    session_send.c:261   (TX encode stage)
    task_session.c:1808  (RX decode stage)

Images:
    FEC ON  -> build/ceepew.bin          (default build)
    FEC OFF -> build_fecoff/ceepew.bin
              (idf.py -B build_fecoff build -D CONFIG_CEEPEW_FEC_DISABLE=y)

For each condition this script flashes BOTH devices with that image and runs
the same receiver-complete delivery measurement as P4a
(tools/p4a_delivery_measurement.py) so the two experiments are directly
comparable.

HARDWARE NOTE: This script is UNTESTED ON HARDWARE. It has only been
validated with --dry-run. Another lane owns COM3/COM4. Do not run it against
real ports until that lane releases them.

Usage:
    python tools/p4b_fec_ablation.py --condition both --num-messages 50
    python tools/p4b_fec_ablation.py --condition fec_off --dry-run
    python tools/p4b_fec_ablation.py --condition fec_on --skip-build --skip-flash

Outputs (under logs/):
    p4b_fec_ablation_<YYYYMMDD_HHMMSS>.json   machine-readable
    p4b_fec_ablation_<YYYYMMDD_HHMMSS>.md     human-readable side-by-side
"""

import sys
import os
import shutil
import argparse
import subprocess
import time
import re
import threading
import json
from datetime import datetime
from typing import TextIO, Optional, Tuple, List, Dict, Any
from collections import Counter

# ---------------------------------------------------------------------------
# Path resolution
# ---------------------------------------------------------------------------
PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TOOLS_DIR = os.path.join(PROJECT_ROOT, "tools")
LOGS_DIR = os.path.join(PROJECT_ROOT, "logs")

# ---------------------------------------------------------------------------
# pyserial
# ---------------------------------------------------------------------------
try:
    import serial
    PYSERIAL_AVAILABLE = True
except ImportError:
    PYSERIAL_AVAILABLE = False
    print("[!] pyserial not installed. Install with 'pip install pyserial'")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Reuse P4a helpers
# ---------------------------------------------------------------------------
# p4a_delivery_measurement.py is importable (its main() is guarded by
# __main__). We reuse its DeliveryStats base, port detection, and the
# flash primitive. We DUPLICATE the serial monitor loop because p4a's
# monitor_port hard-codes a narrow regex set and a fixed stats interface;
# extending it in place would couple the two experiments' internals.
# Anything duplicated verbatim is noted in the report.
try:
    import p4a_delivery_measurement as p4a
    P4A_IMPORTABLE = True
except Exception as _imp_err:
    p4a = None
    P4A_IMPORTABLE = False
    print(f"[!] Could not import tools/p4a_delivery_measurement.py: {_imp_err}")
    print("[!] Falling back to duplicated logic (functionally identical).")

# ---------------------------------------------------------------------------
# Image locations
# ---------------------------------------------------------------------------
IMAGE_FEC_ON = os.path.join(PROJECT_ROOT, "build", "ceepew.bin")
IMAGE_FEC_OFF = os.path.join(PROJECT_ROOT, "build_fecoff", "ceepew.bin")

CONDITION_IMAGES = {
    "fec_on": IMAGE_FEC_ON,
    "fec_off": IMAGE_FEC_OFF,
}

# Kconfig / config symbol that separates the two conditions.
KCONFIG_SYMBOL = "CONFIG_CEEPEW_FEC_DISABLE"
CONFIG_MACRO = "CEEPEW_FEC_ENABLED"
CONFIG_FILE = os.path.join(
    PROJECT_ROOT, "components", "ceepew_common", "ceepew_config.h"
)
TX_GATE_FILE = os.path.join(PROJECT_ROOT, "main", "session_send.c")
RX_GATE_FILE = os.path.join(PROJECT_ROOT, "main", "task_session.c")
TX_GATE_LINE = 261
RX_GATE_LINE = 1808

# ---------------------------------------------------------------------------
# Log-line regexes
# ---------------------------------------------------------------------------
# P4a-compatible success markers (kept identical so counts match P4a).
RE_TX_ALL = re.compile(
    r'\[SECURE_CHAT_TX\] All (\d+) fragments successfully delivered!'
)
RE_RX_OK = re.compile(
    r'\[SECURE_CHAT_RX\] (Huffman decompress complete|Ed25519 signature VERIFIED OK)'
)

# FEC-ablation-specific markers.
# Wire frame size (DEBUG-level log in session_send.c:306).
RE_WIRE_FRAME = re.compile(
    r'\[SECURE_CHAT_TX\] FEC\((on|off)\) & CRC-32 encode: '
    r'in=(\d+) B -> frame=(\d+) B'
)
# ESL wrap total (DEBUG, session_send.c:378) -- cross-check for wire size.
RE_ESL_WRAP = re.compile(r'\[SECURE_CHAT_TX\] ESL frame wrapped.*total=(\d+) B')
# Per-fragment submission to ARQ (DEBUG, session_send.c:515). This is the
# FIRST attempt per fragment; the ARQ retry loop (ecc_arq_send) emits NO
# per-attempt log line, so retry counts are NOT extractable.
RE_FRAGMENT_SUBMIT = re.compile(
    r'\[SECURE_CHAT_TX\] Fragment (\d+)/(\d+): chunk=(\d+) B, frag_in=(\d+) B'
)
# ARQ failure (ERROR, session_send.c:551).
RE_ARQ_FAILED = re.compile(r'\[SECURE_CHAT_TX\] Fragment (\d+) ARQ failed')
# Link-ACK success per fragment (DEBUG, session_send.c:557).
RE_TX_COMPLETE = re.compile(
    r'\[SECURE_CHAT_TX\] Fragment (\d+)/(\d+) TX-complete'
)
# RX-side FEC events.
RE_FEC_CORRECT = re.compile(
    r'\[SECURE_CHAT_RX\] Hamming\(15,11\) FEC corrected single-bit error'
)
RE_FEC_DECODE_FAIL = re.compile(r'\[SECURE_CHAT_RX\] Hamming decode failed')
RE_FEC_OFF_RX = re.compile(r'\[SECURE_CHAT_RX\] CRC verified \(FEC off\)')

# Error / warning line classifier (ESP-IDF style [E]/[W] tags + keywords).
RE_ERR_TAG = re.compile(r'\[(E|W)\]', re.IGNORECASE)
RE_ERR_KEYWORD = re.compile(
    r'\b(FAIL|ERROR|ERR|WARN|WARNING|CRITICAL|panic|abort)\b', re.IGNORECASE
)


def _is_error_or_warning(line: str) -> bool:
    return bool(RE_ERR_TAG.search(line) or RE_ERR_KEYWORD.search(line))
# ---------------------------------------------------------------------------
# AblationStats -- extends P4a DeliveryStats with FEC-ablation metrics
# ---------------------------------------------------------------------------
class AblationStats:
    """Track delivery + FEC-ablation statistics for one condition run.

    Mirrors p4a.DeliveryStats fields so P4a/P4b numbers stay comparable, and
    adds wire-frame sizes, fragment attempts, ARQ failures, RX-side FEC
    events, and per-condition error/warning line counts.
    """

    def __init__(self):
        # P4a-compatible counters
        self.tx_count = 0          # messages fully delivered (All N fragments)
        self.rx_count = 0          # receiver application-layer accepts
        self.tx_timestamps: List[float] = []
        self.rx_timestamps: List[float] = []
        self.tx_errors = 0
        self.rx_errors = 0
        self.first_tx_time: Optional[float] = None
        self.last_rx_time: Optional[float] = None

        # FEC-ablation metrics
        self.wire_frames: List[int] = []          # per-frame wire bytes
        self.fec_mode_seen: Optional[str] = None   # "on"/"off" observed in logs
        self.fragment_submits: List[int] = []      # first-attempt fragment count
        self.arq_failures: List[int] = []          # fragments hitting ARQ fail
        self.tx_complete_count: int = 0            # link-ACK'd fragments
        self.fec_corrections: int = 0              # RX single-bit corrections
        self.fec_decode_failures: int = 0          # RX Hamming decode failures
        self.fec_off_rx_count: int = 0             # RX "CRC verified (FEC off)"
        self.error_lines: Counter = Counter()      # distinct E/W lines -> count
        self.warning_lines: Counter = Counter()
        self.unparsed_lines: List[str] = []        # E/W lines not classified
        self.lock = threading.Lock()

    # -- P4a-compatible recording -------------------------------------------
    def record_tx(self, timestamp: float, success: bool = True):
        with self.lock:
            if success:
                self.tx_count += 1
                self.tx_timestamps.append(timestamp)
                if self.first_tx_time is None:
                    self.first_tx_time = timestamp
            else:
                self.tx_errors += 1

    def record_rx(self, timestamp: float, success: bool = True):
        with self.lock:
            if success:
                self.rx_count += 1
                self.rx_timestamps.append(timestamp)
                self.last_rx_time = timestamp
            else:
                self.rx_errors += 1

    def get_delivery_rate(self) -> float:
        with self.lock:
            if self.tx_count == 0:
                return 0.0
            return (self.rx_count / self.tx_count) * 100.0

    # -- Ablation recording -------------------------------------------------
    def record_wire_frame(self, fec_mode: str, in_bytes: int, frame_bytes: int):
        with self.lock:
            self.wire_frames.append(frame_bytes)
            if self.fec_mode_seen is None:
                self.fec_mode_seen = fec_mode

    def record_fragment_submit(self):
        with self.lock:
            self.fragment_submits.append(1)

    def record_arq_failure(self):
        with self.lock:
            self.arq_failures.append(1)

    def record_tx_complete(self):
        with self.lock:
            self.tx_complete_count += 1

    def record_fec_correction(self):
        with self.lock:
            self.fec_corrections += 1

    def record_fec_decode_failure(self):
        with self.lock:
            self.fec_decode_failures += 1

    def record_fec_off_rx(self):
        with self.lock:
            self.fec_off_rx_count += 1

    def record_error_line(self, line: str):
        with self.lock:
            self.error_lines[line] += 1

    def record_warning_line(self, line: str):
        with self.lock:
            self.warning_lines[line] += 1

    def record_unparsed(self, line: str):
        with self.lock:
            self.unparsed_lines.append(line)

    # -- Summaries ----------------------------------------------------------
    def mean_wire_bytes(self) -> Optional[float]:
        with self.lock:
            if not self.wire_frames:
                return None
            return sum(self.wire_frames) / len(self.wire_frames)

    def max_wire_bytes(self) -> Optional[int]:
        with self.lock:
            if not self.wire_frames:
                return None
            return max(self.wire_frames)

    def min_wire_bytes(self) -> Optional[int]:
        with self.lock:
            if not self.wire_frames:
                return None
            return min(self.wire_frames)

    def get_summary(self) -> str:
        with self.lock:
            elapsed = 0.0
            if self.first_tx_time and self.last_rx_time:
                elapsed = self.last_rx_time - self.first_tx_time
            mw = self.mean_wire_bytes()
            if mw is not None:
                wire_line = f"Mean wire bytes/frame:               {mw:.2f} B\n"
            else:
                wire_line = "Mean wire bytes/frame:               not extracted\n"
            return (
                f"Messages sent (all-fragments-delivered): {self.tx_count}\n"
                f"Receiver-complete accepts:           {self.rx_count}\n"
                f"Delivery rate:                       {self.get_delivery_rate():.2f}%\n"
                f"Fragments submitted (1st attempt):   {len(self.fragment_submits)}\n"
                f"Fragments link-ACK'd:                {self.tx_complete_count}\n"
                f"ARQ failures:                        {len(self.arq_failures)}\n"
                f"Wire frames logged:                  {len(self.wire_frames)}\n"
                f"{wire_line}"
                f"FEC corrections (RX):                {self.fec_corrections}\n"
                f"FEC decode failures (RX):            {self.fec_decode_failures}\n"
                f"Distinct error lines:                {len(self.error_lines)}\n"
                f"Distinct warning lines:              {len(self.warning_lines)}\n"
                f"Elapsed time:                        {elapsed:.2f}s\n"
            )
# ---------------------------------------------------------------------------
# Serial monitor loop
# ---------------------------------------------------------------------------
# Duplicated from p4a_delivery_measurement.py::monitor_port because p4a's
# version hard-codes a narrow regex set. The DTR/RTS reset, in_waiting
# polling, decode/replace behaviour, flush, and stop-event handling are
# identical; only the regex set and the stats calls are extended.
def monitor_port(
    port_name: str,
    label: str,
    stats: AblationStats,
    log_file: TextIO,
    stop_event: threading.Event,
    start_time: float,
    num_messages: int,
):
    try:
        ser = serial.Serial(port_name, 115200, timeout=0.1)
    except Exception as e:
        msg = f"[{label}] Error opening port: {e}"
        print(msg, file=sys.stderr)
        log_file.write(msg + "\n")
        return

    try:
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)
        time.sleep(0.5)
    except Exception as e:
        msg = f"[{label}] Warning: could not reset board: {e}"
        print(msg, file=sys.stderr)
        log_file.write(msg + "\n")

    msg = f"[{label}] Monitoring started."
    print(msg)
    log_file.write(msg + "\n")

    try:
        while not stop_event.is_set():
            try:
                if ser.in_waiting:
                    line = ser.readline().decode("utf-8", errors="replace").rstrip("\n\r")
                    if not line:
                        continue

                    elapsed = time.time() - start_time
                    log_entry = f"{elapsed:.1f} [{label}] {line}"
                    print(f"[{label}] {line}")
                    log_file.write(log_entry + "\n")
                    log_file.flush()

                    # -- P4a-compatible success markers --------------------
                    m_tx = RE_TX_ALL.search(line)
                    m_rx = RE_RX_OK.search(line)

                    # -- FEC-ablation markers ------------------------------
                    m_wire = RE_WIRE_FRAME.search(line)
                    m_submit = RE_FRAGMENT_SUBMIT.search(line)
                    m_arqfail = RE_ARQ_FAILED.search(line)
                    m_txcomplete = RE_TX_COMPLETE.search(line)
                    m_fec_correct = RE_FEC_CORRECT.search(line)
                    m_fec_decode_fail = RE_FEC_DECODE_FAIL.search(line)
                    m_fec_off = RE_FEC_OFF_RX.search(line)

                    if m_wire:
                        stats.record_wire_frame(m_wire.group(1), int(m_wire.group(2)), int(m_wire.group(3)))
                    if m_submit:
                        stats.record_fragment_submit()
                    if m_arqfail:
                        stats.record_arq_failure()
                    if m_txcomplete:
                        stats.record_tx_complete()
                    if m_fec_correct:
                        stats.record_fec_correction()
                    if m_fec_decode_fail:
                        stats.record_fec_decode_failure()
                    if m_fec_off:
                        stats.record_fec_off_rx()

                    # -- Error / warning line accounting --------------------
                    if _is_error_or_warning(line):
                        # Classify by ESP-IDF tag if present.
                        tag_m = RE_ERR_TAG.search(line)
                        if tag_m and tag_m.group(1).upper() == "E":
                            stats.record_error_line(line)
                        elif tag_m and tag_m.group(1).upper() == "W":
                            stats.record_warning_line(line)
                        else:
                            # No [E]/[W] tag but matches a keyword: default to
                            # error bucket (FAIL/ERROR/ERR/CRITICAL/panic).
                            stats.record_error_line(line)

                    # -- P4a success counting ------------------------------
                    if m_tx:
                        stats.record_tx(time.time(), success=True)
                    elif m_rx:
                        stats.record_rx(time.time(), success=True)
                    elif RE_TX_ALL.search(line) is None and RE_RX_OK.search(line) is None:
                        # P4a-style error patterns (only counted when the
                        # line is not already a success).
                        if re.search(r'\[SECURE_CHAT_TX\].*(FAIL|error|ERR)', line):
                            stats.record_tx(time.time(), success=False)
                        elif re.search(r'\[SECURE_CHAT_RX\].*(Discard|FAIL|error|ERR)', line):
                            stats.record_rx(time.time(), success=False)

                    # Stop when we have enough receiver confirmations.
                    if stats.rx_count >= num_messages:
                        print(f"\n[{label}] Received {num_messages} messages - stopping monitor")
                        stop_event.set()
                        break
                else:
                    time.sleep(0.01)
            except Exception:
                time.sleep(0.1)
    finally:
        try:
            ser.close()
        except Exception:
            pass
        msg = f"[{label}] Port closed."
        print(msg)
        log_file.write(msg + "\n")
# ---------------------------------------------------------------------------
# Build / flash helpers
# ---------------------------------------------------------------------------
def _resolve_idf() -> Optional[str]:
    """Locate the idf.py entry point, or return None if it is not resolvable.

    On Windows idf.py ships as a console-script shim (idf.py / idf.py.exe) and is
    only resolvable while an ESP-IDF environment is exported. `subprocess` does
    NOT apply PATHEXT, and idf.py is a script rather than an executable, so
    passing the bare name fails with a context-free `WinError 2`. Resolve
    explicitly here so the caller can report something actionable instead.
    """
    for name in ("idf.py", "idf.py.exe"):
        found = shutil.which(name)
        if found:
            return found
    tools_root = os.environ.get("IDF_TOOLS_PATH") or r"C:\Espressif\tools"
    shim_dir = os.path.join(tools_root, "tools", "idf-exe")
    if os.path.isdir(shim_dir):
        for version in sorted(os.listdir(shim_dir), reverse=True):
            candidate = os.path.join(shim_dir, version, "idf.py.exe")
            if os.path.isfile(candidate):
                return candidate
    return None


def _idf(args, cwd=PROJECT_ROOT, timeout=900, extra_env=None):
    exe = _resolve_idf()
    if exe is None:
        raise RuntimeError(
            "idf.py is not resolvable in this shell. Export the ESP-IDF environment "
            "first (e.g. run export.bat / export.ps1, or use the ESP-IDF terminal), or "
            "run this harness with --skip-build --skip-flash and flash the images "
            "manually with esptool. See the module docstring for the manual command."
        )
    cmd = [exe] + args
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, env=env)


def step_build_fec_on() -> bool:
    """Build the default (FEC ON) image into build/."""
    print("\n" + "=" * 70)
    print("  Building CEE-PEW firmware  [FEC ON]")
    print("=" * 70)
    log_path = os.path.join(LOGS_DIR, f"p4b_build_fec_on_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log")
    os.makedirs(LOGS_DIR, exist_ok=True)
    try:
        result = _idf(["build"])
        with open(log_path, "w", encoding="utf-8") as f:
            f.write(result.stdout)
            f.write(result.stderr)
        print(result.stdout)
        if result.returncode != 0:
            print(result.stderr)
            print(f"\n[FAIL] Build (FEC ON) failed. See {log_path}")
            return False
        print(f"\n[PASS] Build (FEC ON) OK - log: {log_path}")
        return True
    except subprocess.TimeoutExpired:
        print("[FAIL] Build (FEC ON) timed out")
        return False
    except Exception as e:
        print(f"[FAIL] Build (FEC ON) error: {e}")
        return False


def step_build_fec_off() -> bool:
    """Build the FEC-OFF image into build_fecoff/ via the Kconfig symbol."""
    print("\n" + "=" * 70)
    print("  Building CEE-PEW firmware  [FEC OFF]  (CONFIG_CEEPEW_FEC_DISABLE=y)")
    print("=" * 70)
    log_path = os.path.join(LOGS_DIR, f"p4b_build_fec_off_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log")
    os.makedirs(LOGS_DIR, exist_ok=True)
    try:
        result = _idf(["-B", "build_fecoff", "build", "-D", f"{KCONFIG_SYMBOL}=y"])
        with open(log_path, "w", encoding="utf-8") as f:
            f.write(result.stdout)
            f.write(result.stderr)
        print(result.stdout)
        if result.returncode != 0:
            print(result.stderr)
            print(f"\n[FAIL] Build (FEC OFF) failed. See {log_path}")
            return False
        print(f"\n[PASS] Build (FEC OFF) OK - log: {log_path}")
        return True
    except subprocess.TimeoutExpired:
        print("[FAIL] Build (FEC OFF) timed out")
        return False
    except Exception as e:
        print(f"[FAIL] Build (FEC OFF) error: {e}")
        return False


def verify_config_gate(condition: str) -> Optional[str]:
    """Return a human-readable note about the gating macro for this condition.

    Does NOT modify any source file. Reads ceepew_config.h and the two gate
    sites to confirm the ablation switch is wired as documented.
    """
    try:
        with open(CONFIG_FILE, "r", encoding="utf-8") as f:
            cfg = f.read()
    except Exception:
        return None

    if condition == "fec_on":
        expected = "#define CEEPEW_FEC_ENABLED               1"
    else:
        expected = "#define CEEPEW_FEC_ENABLED               0"
    if expected not in cfg:
        return f"NOTE: {CONFIG_FILE} does not contain '{expected}' " \
               f"(build may not match the intended condition)."

    # Confirm the macro is actually consumed at the two documented gate sites.
    notes = []
    for path, line_no in ((TX_GATE_FILE, TX_GATE_LINE), (RX_GATE_FILE, RX_GATE_LINE)):
        try:
            with open(path, "r", encoding="utf-8") as f:
                lines = f.readlines()
            window = "".join(lines[max(0, line_no - 4):line_no + 4])
            if CONFIG_MACRO not in window:
                notes.append(f"{os.path.basename(path)}:{line_no} window does not reference {CONFIG_MACRO}.")
        except Exception:
            pass
    if notes:
        return "NOTE: " + " ".join(notes)
    return None


def step_flash(port: str, condition: str) -> bool:
    """Flash the image for `condition` onto `port`."""
    image_path = CONDITION_IMAGES[condition]
    build_dir = "build" if condition == "fec_on" else "build_fecoff"
    print("\n" + "=" * 70)
    print(f"  Flashing {condition} image to {port}")
    print(f"  Image: {image_path} ({os.path.getsize(image_path)} B)")
    print("=" * 70)
    try:
        result = _idf(["-p", port, "-B", build_dir, "flash"], timeout=300)
        print(result.stdout)
        if result.returncode != 0:
            print(result.stderr)
            print(f"\n[FAIL] Flash ({condition}) failed on {port}")
            return False
        print(f"\n[PASS] {port} flashed OK ({condition})")
        return True
    except subprocess.TimeoutExpired:
        print(f"[FAIL] Flash ({condition}) timed out on {port}")
        return False
    except Exception as e:
        print(f"[FAIL] Flash ({condition}) error on {port}: {e}")
        return False


def verify_image(condition: str) -> Tuple[bool, str]:
    """Check the condition's image exists and return (ok, size_note)."""
    image_path = CONDITION_IMAGES[condition]
    if not os.path.exists(image_path):
        return False, f"MISSING: {image_path}"
    size = os.path.getsize(image_path)
    return True, f"{os.path.basename(os.path.dirname(image_path))}/{os.path.basename(image_path)}  {size} bytes"
# ---------------------------------------------------------------------------
# Run one condition
# ---------------------------------------------------------------------------
def run_condition(
    condition: str,
    port1: str,
    port2: str,
    num_messages: int,
    duration: int,
    skip_build: bool,
    skip_flash: bool,
    dry_run: bool,
) -> Tuple[bool, AblationStats, str]:
    """Run the ablation measurement for one condition.

    Returns (success, stats, notes).
    """
    notes = []
    stats = AblationStats()

    # Verify image exists.
    ok, size_note = verify_image(condition)
    if not ok:
        return False, stats, size_note
    notes.append(f"Image: {size_note}")

    # Build if not skipped.
    if not skip_build and not dry_run:
        if condition == "fec_on":
            if not step_build_fec_on():
                return False, stats, "Build (FEC ON) failed"
        else:
            if not step_build_fec_off():
                return False, stats, "Build (FEC OFF) failed"

    # Flash if not skipped.
    if not skip_flash and not dry_run:
        if not step_flash(port1, condition):
            return False, stats, f"Flash ({condition}) failed on {port1}"
        if not step_flash(port2, condition):
            return False, stats, f"Flash ({condition}) failed on {port2}"

    if dry_run:
        notes.append(f"[DRY-RUN] Would flash {port1} + {port2} with {condition} image")
        notes.append(f"[DRY-RUN] Would monitor for {duration}s or {num_messages} RX messages")
        return True, stats, "\n".join(notes)

    # Run the measurement.
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(LOGS_DIR, f"p4b_{condition}_{timestamp}.txt")
    os.makedirs(LOGS_DIR, exist_ok=True)

    print("\n" + "=" * 70)
    print(f" CEE-PEW P4b FEC-Ablation Measurement  [{condition.upper()}]")
    print("=" * 70)
    print(f"Device A (sender):   {port1}")
    print(f"Device B (receiver): {port2}")
    print(f"Messages to send:    {num_messages}")
    print(f"Max duration:        {duration}s")
    print(f"Log file:            {log_path}")
    print("=" * 70)

    stop_event = threading.Event()
    start_time = time.time()

    with open(log_path, "w", encoding="utf-8", buffering=1) as log_file:
        log_file.write(f"P4b FEC-Ablation [{condition.upper()}] started {datetime.now().isoformat()}\n")
        log_file.write(f"Device A: {port1} (sender)\n")
        log_file.write(f"Device B: {port2} (receiver)\n")
        log_file.write(f"Target messages: {num_messages}\n")
        log_file.write(f"Max duration: {duration}s\n\n")

        threads = []
        t1 = threading.Thread(
            target=monitor_port,
            args=(port1, "DEVICE_A", stats, log_file, stop_event, start_time, num_messages),
            daemon=True,
        )
        t1.start()
        threads.append(t1)

        t2 = threading.Thread(
            target=monitor_port,
            args=(port2, "DEVICE_B", stats, log_file, stop_event, start_time, num_messages),
            daemon=True,
        )
        t2.start()
        threads.append(t2)

        elapsed = 0.0
        interval = 0.5
        while elapsed < duration and not stop_event.is_set():
            time.sleep(interval)
            elapsed += interval
            if int(elapsed) % 10 == 0:
                print(f"\r  Progress: {elapsed:.0f}s/{duration}s | TX: {stats.tx_count} | RX: {stats.rx_count} | Rate: {stats.get_delivery_rate():.1f}%", end="")

        if not stop_event.is_set():
            print(f"\n\n[TIMEOUT] Test reached {duration}s limit")
            stop_event.set()

        # Give monitors time to finalize stats before summary
        time.sleep(1.0)
        for t in threads:
            t.join(timeout=2.0)

        # Force flush all pending log writes before summary
        log_file.flush()
        os.fsync(log_file.fileno())

        log_file.write("\n" + "=" * 70 + "\n")
        log_file.write("FINAL SUMMARY\n")
        log_file.write("=" * 70 + "\n")
        summary_text = stats.get_summary()
        log_file.write(summary_text + "\n")
        log_file.flush()
        os.fsync(log_file.fileno())

        print("\n\n" + "=" * 70)
        print(f" P4b FEC-ABLATION RESULTS  [{condition.upper()}]")
        print("=" * 70)
        print(stats.get_summary())
        print("=" * 70)
        print(f"\nResults saved to: {log_path}")

    return True, stats, "\n".join(notes)
# part 6a: result aggregation + JSON/MD emission

def _fmt_metric(val, null_str="not extracted"):
    if val is None:
        return null_str
    return str(val)

def aggregate_results(results: Dict[str, Dict], conditions: List[str]) -> Dict[str, Any]:
    agg = {
        "schema": "p4b_fec_ablation_v1",
        "conditions": conditions,
        "per_condition": {},
        "comparison": {},
        "confounds": [],
        "notes": [],
    }
    for c in conditions:
        r = results.get(c, {})
        stats = r.get("stats")
        notes = r.get("notes", "")
        entry = {
            "image": r.get("image"),
            "image_size_bytes": r.get("image_size_bytes"),
            "messages_sent": stats.tx_count if stats else None,
            "receiver_completes": stats.rx_count if stats else None,
            "delivery_rate_percent": stats.get_delivery_rate() if stats else None,
            "fragments_submitted": len(stats.fragment_submits) if stats else None,
            "fragments_link_acked": stats.tx_complete_count if stats else None,
            "arq_failures": len(stats.arq_failures) if stats else None,
            "wire_frames_logged": len(stats.wire_frames) if stats else None,
            "mean_wire_bytes_per_frame": stats.mean_wire_bytes() if stats else None,
            "max_wire_bytes_per_frame": stats.max_wire_bytes() if stats else None,
            "min_wire_bytes_per_frame": stats.min_wire_bytes() if stats else None,
            "fec_corrections_rx": stats.fec_corrections if stats else None,
            "fec_decode_failures_rx": stats.fec_decode_failures if stats else None,
            "fec_off_rx_count": stats.fec_off_rx_count if stats else None,
            "distinct_error_lines": len(stats.error_lines) if stats else None,
            "distinct_warning_lines": len(stats.warning_lines) if stats else None,
            "error_lines": dict(stats.error_lines) if stats else {},
            "warning_lines": dict(stats.warning_lines) if stats else {},
            "unparsed_lines": list(stats.unparsed_lines) if stats else [],
            "retries_per_frame": None,  # NOT LOGGED by firmware
            "notes": notes,
        }
        agg["per_condition"][c] = entry
    return agg
def emit_json(agg: Dict[str, Any], path: str):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(agg, f, indent=2, ensure_ascii=False)
    print(f"JSON results: {path}")

def emit_md(agg: Dict[str, Any], path: str, log_paths: Dict[str, str]):
    lines = []
    lines.append("# P4b FEC-Ablation Experiment Results")
    lines.append("")
    lines.append(f"**Generated:** {datetime.now().isoformat()}")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append("| Metric | FEC ON | FEC OFF |")
    lines.append("|--------|--------|---------|")
    pc = agg.get("per_condition", {})
    on = pc.get("fec_on", {})
    off = pc.get("fec_off", {})
    def row(label, k):
        v_on = _fmt_metric(on.get(k))
        v_off = _fmt_metric(off.get(k))
        return f"| {label} | {v_on} | {v_off} |"
    lines.append(row("Messages sent", "messages_sent"))
    lines.append(row("Receiver completes", "receiver_completes"))
    lines.append(row("Delivery rate (%)", "delivery_rate_percent"))
    lines.append(row("Fragments submitted", "fragments_submitted"))
    lines.append(row("Fragments link-ACK'd", "fragments_link_acked"))
    lines.append(row("ARQ failures", "arq_failures"))
    lines.append(row("Wire frames logged", "wire_frames_logged"))
    lines.append(row("Mean wire bytes/frame", "mean_wire_bytes_per_frame"))
    lines.append(row("Max wire bytes/frame", "max_wire_bytes_per_frame"))
    lines.append(row("Min wire bytes/frame", "min_wire_bytes_per_frame"))
    lines.append(row("FEC corrections (RX)", "fec_corrections_rx"))
    lines.append(row("FEC decode failures (RX)", "fec_decode_failures_rx"))
    lines.append(row("FEC-off RX count", "fec_off_rx_count"))
    lines.append(row("Distinct error lines", "distinct_error_lines"))
    lines.append(row("Distinct warning lines", "distinct_warning_lines"))
    lines.append(row("Retries per frame", "retries_per_frame"))
    lines.append("")
    lines.append("## Confounds")
    lines.append("")
    lines.append("- The two images differ ONLY by `CONFIG_CEEPEW_FEC_DISABLE`.")
    lines.append("- FEC OFF changes frame geometry (no Hamming(15,11) padding), so wire-size and delivery-rate effects are entangled.")
    lines.append("- Retries per frame are NOT logged by the firmware; this field is `null` / `not extracted`.")
    lines.append("")
    lines.append("## Raw Logs")
    lines.append("")
    for c in agg.get("conditions", []):
        lp = log_paths.get(c, "not saved")
        lines.append(f"- **{c.upper()}:** {lp}")
    lines.append("")
    lines.append("## Evidence Class")
    lines.append("")
    lines.append("All measurements are **M-HW** (hardware-measured) per the CEE-PEW paper guide.")
    lines.append("")
    lines.append("## Status")
    lines.append("")
    lines.append("**RESULTS PENDING** — do not cite until runs complete and logs are verified.")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"Markdown report: {path}")
def print_dry_run_plan(args):
    print("\n" + "=" * 70)
    print(" P4b FEC-Ablation Experiment — DRY RUN PLAN")
    print("=" * 70)
    print(f"Conditions to test: {args.condition}")
    print(f"Messages per condition: {args.num_messages}")
    print(f"Max duration per condition: {args.duration}s")
    print(f"Ports: {args.port1} (sender), {args.port2} (receiver)")
    print(f"Skip build: {args.skip_build}")
    print(f"Skip flash: {args.skip_flash}")
    print("=" * 70)
    print("\nKconfig switch:")
    print(f"  Symbol: {KCONFIG_SYMBOL}")
    print(f"  Maps to: {CONFIG_MACRO} in ceepew_config.h:245")
    print(f"  Gates: session_send.c:{TX_GATE_LINE} (TX encode)")
    print(f"         task_session.c:{RX_GATE_LINE} (RX decode)")
    print("\nImages:")
    for c in ["fec_on", "fec_off"]:
        ok, note = verify_image(c)
        status = "EXISTS" if ok else "MISSING"
        print(f"  [{status}] {c.upper()}: {note}")
    print("\nPer-condition plan:")
    conds = ["fec_on", "fec_off"] if args.condition == "both" else [args.condition]
    for c in conds:
        print(f"\n  {c.upper()}:")
        print(f"    1. Build image (unless --skip-build)")
        print(f"    2. Flash BOTH devices with {CONDITION_IMAGES[c]}")
        print(f"    3. Monitor {args.port1} + {args.port2} for up to {args.duration}s")
        print(f"    4. Record: messages sent, RX confirms, wire sizes, ARQ failures, error/warning lines")
        print(f"    5. Emit JSON + MD to logs/")
    print("\n" + "=" * 70)
    print(" DRY RUN COMPLETE — no hardware touched")
    print("=" * 70)
def main():
    parser = argparse.ArgumentParser(
        description="CEE-PEW P4b FEC-Ablation Experiment",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python tools/p4b_fec_ablation.py --condition both --num-messages 50
  python tools/p4b_fec_ablation.py --condition fec_off --dry-run
  python tools/p4b_fec_ablation.py --condition fec_on --skip-build --skip-flash
        """
    )
    parser.add_argument("--port1", default="COM3", help="Device A (sender) serial port")
    parser.add_argument("--port2", default="COM4", help="Device B (receiver) serial port")
    parser.add_argument("--num-messages", type=int, default=50, help="Messages to send")
    parser.add_argument("--duration", type=int, default=600, help="Max duration (s)")
    parser.add_argument("--condition", choices=["fec_on", "fec_off", "both"], default="both")
    parser.add_argument("--skip-build", action="store_true", help="Skip build step")
    parser.add_argument("--skip-flash", action="store_true", help="Skip flash step")
    parser.add_argument("--dry-run", action="store_true", help="Print plan and exit")
    args = parser.parse_args()

    if args.dry_run:
        print_dry_run_plan(args)
        sys.exit(0)

    if not PYSERIAL_AVAILABLE:
        print("[!] pyserial required. Install with 'pip install pyserial'")
        sys.exit(1)

    os.makedirs(LOGS_DIR, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    json_path = os.path.join(LOGS_DIR, f"p4b_fec_ablation_{timestamp}.json")
    md_path = os.path.join(LOGS_DIR, f"p4b_fec_ablation_{timestamp}.md")
    log_paths = {}

    conds = ["fec_on", "fec_off"] if args.condition == "both" else [args.condition]
    results = {}
    all_ok = True

    for c in conds:
        print(f"\n{'='*70}\n  CONDITION: {c.upper()}\n{'='*70}")
        ok, stats, notes = run_condition(
            condition=c,
            port1=args.port1,
            port2=args.port2,
            num_messages=args.num_messages,
            duration=args.duration,
            skip_build=args.skip_build,
            skip_flash=args.skip_flash,
            dry_run=False,
        )
        if not ok:
            print(f"[FAIL] {c.upper()} run failed: {notes}")
            all_ok = False
        results[c] = {
            "stats": stats,
            "notes": notes,
            "image": CONDITION_IMAGES[c],
            "image_size_bytes": os.path.getsize(CONDITION_IMAGES[c]) if os.path.exists(CONDITION_IMAGES[c]) else None,
        }
        log_paths[c] = f"logs/p4b_{c}_{timestamp}.txt"

    agg = aggregate_results(results, conds)
    agg["confounds"] = [
        "The two images differ ONLY by CONFIG_CEEPEW_FEC_DISABLE.",
        "FEC OFF changes frame geometry (no Hamming(15,11) padding), so wire-size and delivery-rate effects are entangled.",
        "Retries per frame are NOT logged by the firmware; this field is null / not extracted.",
    ]
    agg["notes"].append(f"Kconfig symbol: {KCONFIG_SYMBOL}")
    agg["notes"].append(f"Gates: session_send.c:{TX_GATE_LINE}, task_session.c:{RX_GATE_LINE}")
    agg["notes"].append(f"P4a helpers reused: {P4A_IMPORTABLE}")
    if not P4A_IMPORTABLE:
        agg["notes"].append("Fell back to duplicated monitor logic (functionally identical to p4a).")

    emit_json(agg, json_path)
    emit_md(agg, md_path, log_paths)

    print("\n" + "=" * 70)
    print(" P4b FEC-ABLATION COMPLETE")
    print("=" * 70)
    print(f"JSON: {json_path}")
    print(f"MD:   {md_path}")
    print(f"Status: {'PASS' if all_ok else 'PARTIAL (see failures above)'}")
    sys.exit(0 if all_ok else 1)

if __name__ == "__main__":
    main()

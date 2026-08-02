#!/usr/bin/env python3
"""
tools/ceepew_pipeline.py

Master Testing & Debugging Pipeline for CEE-PEW firmware.
Orchestrates host unit tests, on-device diagnostics, dual-device pairing tests,
log ingestion & anomaly analysis, fuzz smoke tests, and production readiness checks.

Usage:
  python tools/ceepew_pipeline.py --mode host
  python tools/ceepew_pipeline.py --mode diag [--port COM5] [--duration 60]
  python tools/ceepew_pipeline.py --mode pairing [--port1 COM5 --port2 COM6]
  python tools/ceepew_pipeline.py --mode prod
  python tools/ceepew_pipeline.py --mode fuzz
  python tools/ceepew_pipeline.py --mode all
  python tools/ceepew_pipeline.py --list-ports
"""

import sys
import os
import argparse
import subprocess
import time
from datetime import datetime

# Path resolution
PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TOOLS_DIR = os.path.join(PROJECT_ROOT, "tools")
LOGS_DIR = os.path.join(PROJECT_ROOT, "logs")

# Attempt pyserial import for port discovery
try:
    import serial.tools.list_ports
    PYSERIAL_AVAILABLE = True
except ImportError:
    PYSERIAL_AVAILABLE = False


def detect_ports():
    """Detect connected ESP32 / UART serial ports."""
    if not PYSERIAL_AVAILABLE:
        print("[!] pyserial not installed. Install with 'pip install pyserial' for auto port detection.")
        return []

    ports = serial.tools.list_ports.comports()
    detected = []
    keywords = ["CP210", "CH340", "FTDI", "UART", "USB Serial", "ESP32"]
    for p in ports:
        desc = f"{p.device} - {p.description}"
        if any(kw.lower() in p.description.lower() for kw in keywords) or "COM" in p.device:
            detected.append((p.device, p.description))
    return detected


def list_ports_cmd():
    print("=== Connected Serial Ports ===")
    ports = detect_ports()
    if not ports:
        print("No serial ports detected.")
        return
    for dev, desc in ports:
        print(f"  - {dev}: {desc}")


def find_cmake_bin():
    try:
        res = subprocess.run(["cmake", "--version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if res.returncode == 0:
            return "cmake"
    except (subprocess.SubprocessError, FileNotFoundError):
        pass
    
    # Fallback search on Windows
    espressif_cmake = os.path.join(os.environ.get("SystemDrive", "C:"), "\\Espressif", "tools", "cmake")
    if os.path.exists(espressif_cmake):
        for root, dirs, files in os.walk(espressif_cmake):
            if "cmake.exe" in files:
                return os.path.join(root, "cmake.exe")
    return None


def find_host_gcc():
    candidates = [
        "C:/msys64/ucrt64/bin/gcc.exe",
        "C:/msys64/mingw64/bin/gcc.exe",
        "C:/MinGW/bin/gcc.exe"
    ]
    for candidate in candidates:
        if os.path.exists(candidate):
            return candidate
    return None


def run_host_tests():
    """Compile and execute native host C unit tests via CMake."""
    print("\n============================================")
    print(" CEE-PEW Host C Unit Test Suite")
    print("============================================")

    host_dir = os.path.join(PROJECT_ROOT, "tests", "host")
    build_dir = os.path.join(host_dir, "build_host")

    os.makedirs(build_dir, exist_ok=True)

    cmake_bin = find_cmake_bin()
    if not cmake_bin:
        print("[!] 'cmake' executable not found in PATH or Espressif tools directory.")
        print("    Ensure your C/C++ compiler and CMake environment are active.")
        return False

    cmake_config_cmd = [cmake_bin, "-B", build_dir, "-S", host_dir]
    host_gcc = find_host_gcc()
    if host_gcc:
        cmake_config_cmd.extend([f"-DCMAKE_C_COMPILER={host_gcc}", "-G", "MinGW Makefiles"])

    print("--> Configuring host build (CMake)...")
    res = subprocess.run(cmake_config_cmd, cwd=host_dir)
    if res.returncode != 0:
        print("[FAIL] CMake configuration failed.")
        return False

    print("--> Compiling host tests...")
    res = subprocess.run([cmake_bin, "--build", build_dir], cwd=host_dir)
    if res.returncode != 0:
        print("[FAIL] Host test compilation failed.")
        return False

    # Execute tests using CTest or direct binaries
    print("--> Running Host Tests...")
    test_binaries = [
        "test_security_utils",
        "test_ascon",
        "test_hamming",
        "test_sha256",
        "test_hkdf",
        "test_eddsa",
        "test_huffman",
        "test_session_pairing"
    ]

    passed = 0
    failed = 0

    for test in test_binaries:
        # Check binary location (Windows vs Unix)
        exe_path = os.path.join(build_dir, f"{test}.exe")
        if not os.path.exists(exe_path):
            exe_path = os.path.join(build_dir, test)

        if not os.path.exists(exe_path):
            print(f"  [SKIP] {test} (binary not found at {exe_path})")
            continue

        start_time = time.time()
        test_res = subprocess.run([exe_path], capture_output=True, text=True)
        elapsed = time.time() - start_time

        if test_res.returncode == 0:
            print(f"  [PASS] {test} ({elapsed:.2f}s)")
            passed += 1
        else:
            print(f"  [FAIL] {test} (exit code {test_res.returncode})")
            print(test_res.stdout)
            print(test_res.stderr)
            failed += 1

    print("\n--- HOST TEST RESULTS ---")
    print(f"  Passed: {passed} / {passed + failed}")
    if failed > 0:
        print(f"  Failed: {failed}")
        return False
    return True


def run_production_check():
    """Run production deployment configuration audit."""
    print("\n============================================")
    print(" CEE-PEW Production Configuration Audit")
    print("============================================")

    script_path = os.path.join(TOOLS_DIR, "ceepew_production_check.py")
    res = subprocess.run([sys.executable, script_path], cwd=PROJECT_ROOT)
    return res.returncode == 0


def ingest_log(log_path, session_name=None):
    """Ingest a log file into ceepew_log_pipeline.py and update findings."""
    print("\n--> Ingesting log into signature analysis pipeline...")
    pipeline_script = os.path.join(TOOLS_DIR, "ceepew_log_pipeline.py")
    cmd = [sys.executable, pipeline_script, "ingest", log_path, "--report"]
    if session_name:
        cmd.extend(["--session-name", session_name])

    res = subprocess.run(cmd, cwd=PROJECT_ROOT)
    if res.returncode == 0:
        print("[+] Log findings updated in DEVICE_LOG_FINDINGS.md")
    else:
        print("[!] Log ingestion encountered an issue.")


def run_diagnostics(port=None, duration=60, full_clean=False):
    """Execute on-device diagnostic build & test run."""
    print("\n============================================")
    print(" CEE-PEW On-Device Diagnostic Run")
    print("============================================")

    # Auto detect port if not specified
    if not port:
        detected = detect_ports()
        if detected:
            port = detected[0][0]
            print(f"--> Auto-detected target port: {port}")
        else:
            port = "COM5" # Default fallback
            print(f"--> Using default port: {port}")

    os.makedirs(LOGS_DIR, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_filename = f"diag_{timestamp}.txt"
    log_path = os.path.join(LOGS_DIR, log_filename)

    ps_script = os.path.join(TOOLS_DIR, "ceepew_diagnose.ps1")
    cmd = ["powershell", "-ExecutionPolicy", "Bypass", "-File", ps_script,
           "-Mode", "Diagnose", "-Port", port, "-Duration", str(duration)]
    if full_clean:
        cmd.append("-FullClean")

    print(f"--> Launching diagnostic run on {port}...")
    res = subprocess.run(cmd, cwd=PROJECT_ROOT)

    # If log file was captured, ingest it
    if os.path.exists(log_path):
        ingest_log(log_path, session_name=f"diag_{timestamp}")
    else:
        # Look for any recent log file in logs/
        recent_logs = sorted([os.path.join(LOGS_DIR, f) for f in os.listdir(LOGS_DIR) if f.endswith(".txt")])
        if recent_logs:
            ingest_log(recent_logs[-1], session_name=f"diag_{timestamp}")

    return res.returncode == 0


def run_pairing_test(port1=None, port2=None, duration=120, skip_flash=False):
    """Execute two-device pairing integration test."""
    print("\n============================================")
    print(" CEE-PEW Two-Device Pairing Integration Test")
    print("============================================")

    # Auto detect ports if needed
    if not port1 or not port2:
        detected = detect_ports()
        if len(detected) >= 2:
            port1 = detected[0][0]
            port2 = detected[1][0]
            print(f"--> Auto-detected pairing ports: Device A = {port1}, Device B = {port2}")
        else:
            port1 = port1 or "COM5"
            port2 = port2 or "COM6"
            print(f"--> Using ports: Device A = {port1}, Device B = {port2}")

    os.makedirs(LOGS_DIR, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_filename = f"pairing_{timestamp}.txt"
    log_path = os.path.join(LOGS_DIR, log_filename)

    ps_script = os.path.join(TOOLS_DIR, "ceepew_diagnose.ps1")
    cmd = ["powershell", "-ExecutionPolicy", "Bypass", "-File", ps_script,
           "-Mode", "Pairing", "-Duration", str(duration),
           "-Port1", port1, "-Port2", port2]
    if skip_flash:
        cmd.append("-SkipFlash")

    print(f"--> Launching pairing test on {port1} & {port2}...")
    res = subprocess.run(cmd, cwd=PROJECT_ROOT)

    if os.path.exists(log_path):
        ingest_log(log_path, session_name=f"pairing_{timestamp}")
    else:
        recent_logs = sorted([os.path.join(LOGS_DIR, f) for f in os.listdir(LOGS_DIR) if f.startswith("pairing_") and f.endswith(".txt")])
        if recent_logs:
            ingest_log(recent_logs[-1], session_name=f"pairing_{timestamp}")

    return res.returncode == 0


def run_fuzz_tests():
    """Execute libFuzzer smoke test harnesses."""
    print("\n============================================")
    print(" CEE-PEW Fuzz Harness Smoke Test")
    print("============================================")

    fuzz_script = os.path.join(TOOLS_DIR, "run_fuzz_harnesses.sh")
    if not os.path.exists(fuzz_script):
        print("[!] Fuzz script not found.")
        return False

    # Check bash availability
    try:
        res = subprocess.run(["bash", fuzz_script], cwd=PROJECT_ROOT)
        return res.returncode == 0
    except (subprocess.SubprocessError, FileNotFoundError):
        print("[!] 'bash' executable not available in environment for fuzz harnesses.")
        print("    Run this command in WSL, Linux, or a Git Bash terminal.")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="CEE-PEW Master Testing & Debugging Pipeline Runner",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--mode",
        choices=["host", "diag", "pairing", "prod", "fuzz", "all"],
        help="Test mode to execute: host (unit tests), diag (on-device diagnostics), pairing (two-device test), prod (production audit), fuzz (libFuzzer), all (full suite)"
    )
    parser.add_argument("--port", help="Serial port for single-device diagnostic mode (e.g. COM5)")
    parser.add_argument("--port1", help="Device A serial port for pairing mode (e.g. COM5)")
    parser.add_argument("--port2", help="Device B serial port for pairing mode (e.g. COM6)")
    parser.add_argument("--duration", type=int, default=60, help="Test monitoring duration in seconds (default: 60)")
    parser.add_argument("--full-clean", action="store_true", help="Force full build directory clean before diagnostic build")
    parser.add_argument("--skip-flash", action="store_true", help="Skip flashing step during pairing test")
    parser.add_argument("--list-ports", action="store_true", help="List all connected serial ports and exit")

    args = parser.parse_args()

    if args.list_ports:
        list_ports_cmd()
        return

    if not args.mode:
        parser.print_help()
        sys.exit(1)

    print("==========================================================")
    print(" CEE-PEW Rapid Testing & Debugging Pipeline")
    print(" Time:", datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
    print("==========================================================")

    success = True

    if args.mode == "host":
        success = run_host_tests()
    elif args.mode == "diag":
        success = run_diagnostics(port=args.port, duration=args.duration, full_clean=args.full_clean)
    elif args.mode == "pairing":
        success = run_pairing_test(port1=args.port1, port2=args.port2, duration=args.duration, skip_flash=args.skip_flash)
    elif args.mode == "prod":
        success = run_production_check()
    elif args.mode == "fuzz":
        success = run_fuzz_tests()
    elif args.mode == "all":
        print("\n[+] Running Full Verification Suite...")
        h_ok = run_host_tests()
        p_ok = run_production_check()
        d_ok = run_diagnostics(port=args.port, duration=args.duration)
        success = h_ok and p_ok and d_ok

    print("\n==========================================================")
    if success:
        print(" PIPELINE EXECUTION: SUCCESS [ALL STAGES PASSED]")
        sys.exit(0)
    else:
        print(" PIPELINE EXECUTION: COMPLETED WITH ISSUES / FAILURES")
        sys.exit(1)


if __name__ == "__main__":
    main()

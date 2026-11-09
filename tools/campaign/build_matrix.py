#!/usr/bin/env python3
"""tools/campaign/build_matrix.py - build every firmware configuration the
measurement campaigns need, and record what each one produced.

The campaign compares configurations, so each configuration gets its own
build directory. Mixing configurations in one directory silently reuses
stale objects, which is how a "FEC off" run ends up running a "FEC on"
image. This script therefore never reuses a build directory and records the
SHA-256 of every resulting image.

On Windows the ESP-IDF build regenerates build.ninja in place, and a
concurrently running ninja or an indexer can hold the file, which fails
with "ninja: error: failed recompaction: Permission denied". The script
therefore runs one configuration at a time and accepts --build-dir
overrides for a clean directory.

Configurations
  default  shipping Full profile, Hamming FEC compiled out, development
           mode off. The image that goes to production.
  bench    Full profile, FEC out, development mode on, with the scripted
           TX campaign, the GPIO14 timing instrumentation and the
           same-board AEAD baseline enabled. The X1, X2 and X8 workhorse.
  lite     Lite profile (no outer box, no per-fragment signature), FEC out,
           development mode on, bench flags on. The profile comparison.
  fec      Full profile, Hamming FEC compiled in, development mode on,
           bench flags on. The FEC ablation arm.
  prod     Full profile, FEC out, development mode off, used by the
           production readiness audit.

Configuration is applied through ``SDKCONFIG_DEFAULTS``, never through
``-D CONFIG_*``. ESP-IDF generates ``sdkconfig`` from the defaults files and
the existing ``sdkconfig``; a CMake cache variable of the same name is
ignored, so a ``-D CONFIG_*`` matrix silently builds the same image five
times. Each configuration therefore owns both its ``sdkconfig`` and its
overlay file, and every build is verified against the symbols the
configuration is supposed to produce before it is recorded as OK.

Usage:
    python tools/campaign/build_matrix.py
    python tools/campaign/build_matrix.py --configs default bench
    python tools/campaign/build_matrix.py --configs lite --build-dir lite build_lite3
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
LOG_DIR = os.path.join(PROJECT_ROOT, "logs", "build")

BENCH_FLAGS = (
    "CONFIG_CEEPEW_DEVELOPMENT_MODE=y",
    "CONFIG_CEEPEW_BENCH_58B=y",
    "CONFIG_CEEPEW_BENCH_TIMING=y",
    "CONFIG_CEEPEW_BENCH_AEAD=y",
    # Development mode compiles in the test component and the benchmark
    # module. The .bss shortfall is fixed in crypto_bench.h, where the result
    # structure carried sixteen case slots for a module that records at most
    # eleven. This stack line is not that fix: the main task stack is taken
    # from the heap, so it never appears in .bss and changing it moved the
    # linker not at all. It is retained only because every bench-enabled arm
    # carries it and every manifest binds the resulting sdkconfig hash; main
    # returns its stack after initialisation, before any timed region runs.
    "CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192",
)

CONFIGS: Dict[str, Dict[str, Any]] = {
    "default": {
        "build_dir": "build",
        "flags": ("CONFIG_CEEPEW_FEC_DISABLE=y",),
        "expect_present": (),
        "expect_absent": ("ecc_hamming_encode", "ecc_hamming_decode", "crypto_bench_run"),
        "purpose": "shipping Full profile, FEC compiled out, development mode off",
    },
    "bench": {
        "build_dir": "build_bench3",
        "flags": BENCH_FLAGS + ("CONFIG_CEEPEW_FEC_DISABLE=y",),
        "expect_present": ("crypto_bench_run",),
        "expect_absent": ("ecc_hamming_encode", "ecc_hamming_decode"),
        "purpose": "campaign workhorse: Full profile, FEC out, bench flags on",
    },
    "lite": {
        "build_dir": "build_lite2",
        "flags": BENCH_FLAGS + ("CONFIG_CEEPEW_PROFILE_LITE=y", "CONFIG_CEEPEW_FEC_DISABLE=y"),
        "expect_present": ("crypto_bench_run",),
        "expect_absent": ("ecc_hamming_encode", "ecc_hamming_decode"),
        "purpose": "Lite profile (no outer box, no per-fragment signature), FEC out",
    },
    "fec": {
        "build_dir": "build_fec2",
        "flags": BENCH_FLAGS + ("CONFIG_CEEPEW_FEC_DISABLE=n",),
        "expect_present": ("crypto_bench_run", "ecc_hamming_encode", "ecc_hamming_decode"),
        "expect_absent": (),
        "purpose": "FEC ablation arm: Hamming(15,11) compiled in",
    },
    "prod": {
        "build_dir": "build_prod",
        "flags": ("CONFIG_CEEPEW_DEVELOPMENT_MODE=n", "CONFIG_CEEPEW_FEC_DISABLE=y"),
        "expect_present": (),
        "expect_absent": ("ecc_hamming_encode", "ecc_hamming_decode", "crypto_bench_run"),
        "purpose": "production readiness audit image",
    },
}


def sha256_file(path: str) -> str:
    if not os.path.isfile(path):
        return ""
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def write_powershell_script(path: str, idf_args: List[str]) -> None:
    """Write a self-contained PowerShell driver for one build.

    A script file is used instead of an inline command because the inline
    form is parsed by cmd.exe first, which mangles the pipeline in the
    IDF export step.
    """
    idf_path = os.environ.get("IDF_PATH", r"C:\esp\v6.1\esp-idf")
    tools_path = os.environ.get("IDF_TOOLS_PATH", r"C:\Espressif\tools")
    python_env = os.environ.get(
        "IDF_PYTHON_ENV_PATH", r"C:\Espressif\tools\python_env\idf6.0_py3.13_env"
    )
    export = os.path.join(idf_path, "export.ps1")
    quoted = " ".join("'%s'" % arg.replace("'", "''") for arg in idf_args)
    body = [
        "$ErrorActionPreference = 'Continue'",
        "$env:IDF_PATH = '%s'" % idf_path,
        "$env:IDF_TOOLS_PATH = '%s'" % tools_path,
        "$env:IDF_PYTHON_ENV_PATH = '%s'" % python_env,
        "if (Test-Path '%s') { . '%s' | Out-Null }" % (export, export),
        "Set-Location '%s'" % PROJECT_ROOT,
        "& idf.py %s" % quoted,
        "exit $LASTEXITCODE",
        "",
    ]
    with open(path, "w", encoding="utf-8", newline="\r\n") as fh:
        fh.write("\n".join(body))


def write_overlay(name: str, flags) -> str:
    """Write the Kconfig overlay for one configuration and return its path.

    ``SDKCONFIG_DEFAULTS`` takes a semicolon-separated list applied in order,
    so a later entry wins. The overlay therefore carries only the deltas for
    this configuration, on top of the project's own ``sdkconfig.defaults``.
    """
    os.makedirs(LOG_DIR, exist_ok=True)
    path = os.path.join(LOG_DIR, "cfg_%s.defaults" % name)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# generated by tools/campaign/build_matrix.py for %r\n" % name)
        for flag in flags:
            fh.write("%s\n" % flag)
    return path


def sdkconfig_symbols(header: str) -> Dict[str, bool]:
    """Read the set/unset state of every CONFIG_CEEPEW_* symbol in a header."""
    found: Dict[str, bool] = {}
    if not os.path.isfile(header):
        return found
    with open(header, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("#define CONFIG_CEEPEW_"):
                name = line.split()[1]
                found[name] = True
            elif line.startswith("#define ") and "CEEPEW" in line:
                found[line.split()[1]] = True
    return found


def elf_symbols(elf: str, names) -> Dict[str, bool]:
    """Report which of ``names`` appear in the ELF symbol table."""
    result = {n: False for n in names}
    if not os.path.isfile(elf):
        return result
    nm = find_nm()
    if not nm:
        return result
    out = subprocess.run([nm, elf], capture_output=True, text=True, errors="replace")
    text = out.stdout or ""
    for name in names:
        result[name] = name in text
    return result


def find_nm() -> Optional[str]:
    """Locate the Xtensa nm binary shipped with the current toolchain."""
    tools_path = os.environ.get("IDF_TOOLS_PATH", r"C:\Espressif\tools")
    root = os.path.join(tools_path, "tools", "xtensa-esp-elf")
    if not os.path.isdir(root):
        return None
    for entry in sorted(os.listdir(root), reverse=True):
        candidate = os.path.join(
            root, entry, "xtensa-esp-elf", "bin", "xtensa-esp32-elf-nm.exe"
        )
        if os.path.isfile(candidate):
            return candidate
    return None


def build_one(name: str, cfg: Dict[str, Any], build_dir: Optional[str], dry_run: bool) -> Dict[str, Any]:
    """Build one configuration and record the result."""
    target_dir = build_dir or cfg["build_dir"]
    idf_args = ["-B", target_dir, "-D", "IDF_TARGET=esp32"]
    # The shipping configuration is the project's own sdkconfig; every other
    # configuration gets a private sdkconfig and a defaults overlay. Passing
    # "-D CONFIG_X=y" instead would be silently ignored by ESP-IDF's config
    # generator, which is how a five-way matrix becomes five copies of one
    # image.
    overlay = write_overlay(name, cfg["flags"])
    if name != "default":
        abs_overlay = os.path.abspath(overlay)
        base_defaults = os.path.abspath(os.path.join(PROJECT_ROOT, "sdkconfig.defaults"))
        private_sdkconfig = os.path.abspath(os.path.join(PROJECT_ROOT, target_dir, "sdkconfig"))
        # A defaults file only seeds a sdkconfig that does not yet exist. If the
        # build directory already carries one, its values win and the overlay is
        # silently ignored, which is the same failure mode as "-D CONFIG_*".
        # Deleting it makes the overlay authoritative on every run.
        if os.path.isfile(private_sdkconfig):
            os.remove(private_sdkconfig)
        idf_args += ["-D", "SDKCONFIG=%s" % private_sdkconfig]
        idf_args += ["-D", "SDKCONFIG_DEFAULTS=%s;%s" % (base_defaults, abs_overlay)]
    idf_args += ["build"]

    os.makedirs(LOG_DIR, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    log_path = os.path.join(LOG_DIR, "matrix_%s_%s.log" % (name, stamp))
    ps1_path = os.path.join(LOG_DIR, "matrix_%s_%s.ps1" % (name, stamp))

    record: Dict[str, Any] = {
        "config": name,
        "build_dir": target_dir,
        "flags": list(cfg["flags"]),
        "overlay": os.path.relpath(overlay, PROJECT_ROOT).replace("\\", "/"),
        "purpose": cfg["purpose"],
        "log": os.path.relpath(log_path, PROJECT_ROOT).replace("\\", "/"),
        "started_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }

    if dry_run:
        record["result"] = "dry-run"
        record["command"] = "idf.py " + " ".join(idf_args)
        return record

    write_powershell_script(ps1_path, idf_args)
    cmd = [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        ps1_path,
    ]
    started = time.time()
    with open(log_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# idf.py " + " ".join(idf_args) + "\n")
        fh.flush()
        proc = subprocess.run(
            cmd,
            cwd=PROJECT_ROOT,
            stdout=fh,
            stderr=subprocess.STDOUT,
            shell=False,
        )
    elapsed = time.time() - started

    bin_path = os.path.join(PROJECT_ROOT, target_dir, "ceepew.bin")
    elf_path = os.path.join(PROJECT_ROOT, target_dir, "ceepew.elf")
    header = os.path.join(PROJECT_ROOT, target_dir, "config", "sdkconfig.h")

    record["returncode"] = proc.returncode
    record["seconds"] = round(elapsed, 1)
    record["firmware"] = os.path.relpath(bin_path, PROJECT_ROOT).replace("\\", "/")
    record["firmware_bytes"] = os.path.getsize(bin_path) if os.path.isfile(bin_path) else 0
    record["firmware_sha256"] = sha256_file(bin_path)

    # Verification, not trust: a build that compiled the wrong configuration
    # is a failed build even when the compiler exited zero.
    present = elf_symbols(elf_path, cfg["expect_present"])
    absent = elf_symbols(elf_path, cfg["expect_absent"])
    record["symbols_present"] = present
    record["symbols_absent"] = absent
    record["sdkconfig"] = os.path.relpath(
        os.path.join(PROJECT_ROOT, target_dir, "sdkconfig"), PROJECT_ROOT
    ).replace("\\", "/")
    record["sdkconfig_sha256"] = sha256_file(os.path.join(PROJECT_ROOT, target_dir, "sdkconfig"))

    problems = []
    if proc.returncode != 0:
        problems.append("compiler exit %d" % proc.returncode)
    for symbol, ok in present.items():
        if not ok:
            problems.append("expected symbol %s missing" % symbol)
    for symbol, ok in absent.items():
        if ok:
            problems.append("symbol %s present but should be absent" % symbol)

    # Development-mode and profile flags are visible in the generated header.
    if os.path.isfile(header):
        symbols = sdkconfig_symbols(header)
        wants_dev = "CONFIG_CEEPEW_DEVELOPMENT_MODE=y" in cfg["flags"]
        has_dev = symbols.get("CONFIG_CEEPEW_DEVELOPMENT_MODE", False)
        if wants_dev != has_dev:
            problems.append(
                "development mode %s but configuration wanted %s"
                % ("on" if has_dev else "off", "on" if wants_dev else "off")
            )
        wants_fec = "CONFIG_CEEPEW_FEC_DISABLE=y" in cfg["flags"]
        has_fec = symbols.get("CONFIG_CEEPEW_FEC_DISABLE", False)
        if wants_fec != has_fec:
            problems.append(
                "FEC_DISABLE %s but configuration wanted %s"
                % ("on" if has_fec else "off", "on" if wants_fec else "off")
            )
        wants_lite = "CONFIG_CEEPEW_PROFILE_LITE=y" in cfg["flags"]
        has_lite = symbols.get("CONFIG_CEEPEW_PROFILE_LITE", False)
        if wants_lite != has_lite:
            problems.append(
                "Lite profile %s but configuration wanted %s"
                % ("on" if has_lite else "off", "on" if wants_lite else "off")
            )
    else:
        problems.append("no generated sdkconfig.h at %s" % header)

    record["problems"] = problems
    record["result"] = "OK" if not problems else "FAILED"
    return record


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Build every campaign firmware configuration.")
    parser.add_argument("--configs", nargs="*", default=list(CONFIGS), help="configurations to build")
    parser.add_argument("--build-dir", default="", help="override the build directory (clean directory recommended)")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--out-json", default="", help="write the build matrix summary as JSON")
    args = parser.parse_args(argv)

    unknown = [name for name in args.configs if name not in CONFIGS]
    if unknown:
        parser.error("unknown configuration(s): %s" % ", ".join(unknown))

    records: List[Dict[str, Any]] = []
    for name in args.configs:
        print("=== building %s (%s)" % (name, CONFIGS[name]["purpose"]), flush=True)
        record = build_one(name, CONFIGS[name], args.build_dir or None, args.dry_run)
        records.append(record)
        print(
            "    %s  %s  %s bytes  %s"
            % (
                name,
                record.get("result", "?"),
                record.get("firmware_bytes", 0),
                record.get("firmware_sha256", "")[:16],
            ),
            flush=True,
        )
        for problem in record.get("problems", []):
            print("      ! %s" % problem, flush=True)

    out_path = args.out_json or os.path.join(
        LOG_DIR,
        "build_matrix_%s.json" % datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"),
    )
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump({"records": records}, fh, indent=2, sort_keys=True)
        fh.write("\n")

    failed = [r["config"] for r in records if r.get("result") == "FAILED"]
    print(
        "summary: %d built, %d failed (%s)"
        % (len(records) - len(failed), len(failed), ", ".join(failed) or "none")
    )
    print("wrote %s" % os.path.relpath(out_path, PROJECT_ROOT).replace("\\", "/"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

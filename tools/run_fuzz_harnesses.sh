#!/usr/bin/env bash
#
# tools/run_fuzz_harnesses.sh
#
# Compiles each CEE-PEW fuzz harness with clang + libFuzzer and runs a
# brief smoke test (60 seconds per harness).  Fails on any crash or
# build error.
#
# Prerequisites: clang (with -fsanitize=fuzzer support), libclang-rt-dev.
#
# Usage:
#   bash tools/run_fuzz_harnesses.sh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FUZZ_DIR="${ROOT}/fuzz"

# ESP-IDF path (set by Docker env or default)
IDF_PATH="${IDF_PATH:-/opt/esp/idf}"

# Harness directory   → component source files needed
declare -A HARNESSES
HARNESSES[fuzz_ascon]="components/crypto/crypto_ascon.c"
HARNESSES[fuzz_sha256]="components/crypto/crypto_sha256.c"
HARNESSES[fuzz_hamming]="components/ecc/ecc_hamming.c"
HARNESSES[fuzz_hkdf]="components/crypto/crypto_hkdf.c components/crypto/crypto_sha256.c"
HARNESSES[fuzz_arq]="components/ecc/ecc_arq.c"

# Include paths common to all harnesses
COMMON_INCLUDES="-I${ROOT}/components/ceepew_common -I${ROOT}/components/hal -I${ROOT}/components/crypto -I${ROOT}/components/ecc -I${ROOT}/components/transport"

# Compile definitions for host-side builds (stubs for ESP-IDF macros)
COMMON_DEFS="-DWIFI_PS_MIN_MODEM=0 -DWIFI_PS_NONE=0 -DCONFIG_CEEPEW_DEVELOPMENT_MODE=1"

# Per-harness extra include paths (e.g. for ESP-IDF dependencies)
declare -A EXTRA_INCLUDES
EXTRA_INCLUDES[fuzz_hamming]="-I${ROOT}/tests/host/include -I${IDF_PATH}/components/esp_common/include -I${IDF_PATH}/components/log/include"
EXTRA_INCLUDES[fuzz_arq]="-I${ROOT}/tests/host/include -I${IDF_PATH}/components/esp_common/include -I${IDF_PATH}/components/log/include -I${IDF_PATH}/components/freertos/FreeRTOS-Kernel/include -I${IDF_PATH}/components/freertos/esp_additions/include -I${IDF_PATH}/components/esp_timer/include -I${IDF_PATH}/components/esp_system/include -I${IDF_PATH}/components/esp_hw_support/include -I${IDF_PATH}/components/esp_rom/include -I${IDF_PATH}/components/soc/include -I${IDF_PATH}/components/hal/include -I${IDF_PATH}/components/esp_random/include"

# Working directory for build artefacts
BUILD_DIR="${FUZZ_DIR}/build"
mkdir -p "${BUILD_DIR}"

echo "============================================"
echo " CEE-PEW Fuzz Harness Smoke Tests"
echo "============================================"

TOTAL=0
PASSED=0
FAILED=0

for HARNESS_DIR in "${!HARNESSES[@]}"; do
    TOTAL=$((TOTAL + 1))
    HARNESS_SRC="${FUZZ_DIR}/${HARNESS_DIR}/${HARNESS_DIR}.c"
    HARNESS_BIN="${BUILD_DIR}/${HARNESS_DIR}"

    echo ""
    echo "--- ${HARNESS_DIR} ---"

    if [[ ! -f "${HARNESS_SRC}" ]]; then
        echo "  [FAIL] Harness source not found: ${HARNESS_SRC}"
        FAILED=$((FAILED + 1))
        continue
    fi

    # Extra includes for this harness
    HARNESS_EXTRA="${EXTRA_INCLUDES[$HARNESS_DIR]:-}"
    ALL_INCLUDES="${COMMON_DEFS} ${COMMON_INCLUDES} ${HARNESS_EXTRA}"

    # Collect component .o files
    OBJ_FILES=""
    NEED_REBUILD=0
    for SRC_REL in ${HARNESSES[$HARNESS_DIR]}; do
        SRC="${ROOT}/${SRC_REL}"
        OBJ="${BUILD_DIR}/$(basename "${SRC_REL}" .c).o"
        if [[ ! -f "${OBJ}" ]] || [[ "${SRC}" -nt "${OBJ}" ]]; then
            NEED_REBUILD=1
            echo "  Compiling: ${SRC_REL}"
            clang -fsanitize=fuzzer -g -O1 ${ALL_INCLUDES} -c "${SRC}" -o "${OBJ}"
        fi
        OBJ_FILES="${OBJ_FILES} ${OBJ}"
    done

    # Build harness binary if needed
    if [[ ! -f "${HARNESS_BIN}" ]] || [[ "${HARNESS_SRC}" -nt "${HARNESS_BIN}" ]] || [[ "${NEED_REBUILD}" -eq 1 ]]; then
        echo "  Linking:   ${HARNESS_DIR}"
        clang -fsanitize=fuzzer -g -O1 ${ALL_INCLUDES} \
            "${HARNESS_SRC}" ${OBJ_FILES} \
            -o "${HARNESS_BIN}"
    fi

    # Smoke test: run for 60 seconds, generate a small corpus on the fly
    echo "  Running:   ${HARNESS_DIR} (60s smoke test)"
    set +e
    "${HARNESS_BIN}" -max_total_time=60 -max_len=512 -runs=1000000 \
        -artifact_prefix="${BUILD_DIR}/" 2>&1 | tail -5
    EXIT_CODE=$?
    set -e

    if [[ ${EXIT_CODE} -eq 0 ]]; then
        echo "  [PASS] ${HARNESS_DIR}"
        PASSED=$((PASSED + 1))
    else
        echo "  [FAIL] ${HARNESS_DIR} (exit code ${EXIT_CODE})"
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo "============================================"
echo " Results: ${PASSED}/${TOTAL} passed, ${FAILED} failed"
echo "============================================"

if [[ ${FAILED} -ne 0 ]]; then
    exit 1
fi

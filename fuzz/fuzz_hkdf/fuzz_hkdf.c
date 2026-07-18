/* fuzz/fuzz_hkdf/fuzz_hkdf.c — HKDF-SHA256 fuzz target
 *
 * Fuzzes crypto_hkdf_derive() with all parameters drawn from fuzzer input.
 * Verifies that HKDF is deterministic (calling twice with same inputs
 * produces the same output).
 *
 * Build (libFuzzer):
 *   clang -fsanitize=fuzzer -I../../components/crypto -I../../components/ceepew_common \
 *         -I../../components/hal \
 *         -c ../../components/crypto/crypto_hkdf.c -o crypto_hkdf.o
 *   clang -fsanitize=fuzzer -I../../components/crypto -I../../components/ceepew_common \
 *         -I../../components/hal -c ../../components/crypto/crypto_sha256.c \
 *         -o crypto_sha256.o
 *   clang -fsanitize=fuzzer -I../../components/crypto -I../../components/ceepew_common \
 *         -I../../components/hal fuzz_hkdf.c crypto_hkdf.o crypto_sha256.o \
 *         -o fuzz_hkdf
 *
 * Build (AFL):
 *   afl-clang-fast ... (same flags, link with __AFL_FUZZ_INIT)
 */

#include <stdint.h>
#include <string.h>
#include "ceepew_assert.h"

CeePewErr_t crypto_hkdf_derive(const uint8_t *ikm, uint8_t ikm_len,
                               const uint8_t *salt, uint8_t salt_len,
                               const uint8_t *info, uint8_t info_len,
                               uint8_t *out, uint8_t out_len);

#ifndef CEEPEW_OK
#define CEEPEW_OK 0
#endif

/* Minimum bytes needed: 4 header bytes + at least 1 byte for each region */
#define MIN_DATA 7U

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < MIN_DATA) return 0;

    /* Layout of fuzzer input:
     *   [0]      = ikm_len
     *   [1]      = salt_len
     *   [2]      = info_len
     *   [3]      = out_len (clamped to 64)
     *   [4..]    = concatenated ikm || salt || info
     */
    uint8_t ikm_len  = data[0];
    uint8_t salt_len = data[1];
    uint8_t info_len = data[2];
    uint8_t out_len  = data[3];
    if (out_len > 64U) out_len = 64U;

    size_t total_needed = (size_t)ikm_len + (size_t)salt_len + (size_t)info_len;
    if (total_needed > size - 4U) return 0;

    size_t pos = 4U;
    const uint8_t *ikm  = data + pos; pos += ikm_len;
    const uint8_t *salt = data + pos; pos += salt_len;
    const uint8_t *info = data + pos;

    uint8_t out1[64];
    uint8_t out2[64];
    memset(out1, 0xAA, sizeof(out1));
    memset(out2, 0xBB, sizeof(out2));

    CeePewErr_t err = crypto_hkdf_derive(
        ikm, ikm_len, salt, salt_len, info, info_len,
        out1, out_len);

    if (err != CEEPEW_OK) return 0;

    /* Determinism check */
    err = crypto_hkdf_derive(
        ikm, ikm_len, salt, salt_len, info, info_len,
        out2, out_len);

    if (err != CEEPEW_OK) __builtin_trap();
    if (memcmp(out1, out2, out_len) != 0) __builtin_trap();

    return 0;
}

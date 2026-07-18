/* fuzz/fuzz_sha256/fuzz_sha256.c — SHA-256 fuzz target
 *
 * Fuzzes crypto_sha256_compute() with arbitrary input lengths.
 *
 * Build (libFuzzer):
 *   clang -fsanitize=fuzzer -I../../components/crypto -I../../components/ceepew_common \
 *         -I../../components/hal -c ../../components/crypto/crypto_sha256.c \
 *         -o crypto_sha256.o
 *   clang -fsanitize=fuzzer -I../../components/crypto -I../../components/ceepew_common \
 *         -I../../components/hal fuzz_sha256.c crypto_sha256.o \
 *         -o fuzz_sha256
 *
 * Build (AFL):
 *   afl-clang-fast ... (same flags, link with __AFL_FUZZ_INIT)
 */

#include <stdint.h>
#include <string.h>
#include "ceepew_assert.h"

CeePewErr_t crypto_sha256_compute(const uint8_t *in, uint32_t len, uint8_t out[32]);

#ifndef CEEPEW_OK
#define CEEPEW_OK 0
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65535U) return 0;  /* keep within uint32_t len */

    uint8_t digest[32];
    CeePewErr_t err = crypto_sha256_compute(data, (uint32_t)size, digest);
    if (err != CEEPEW_OK) __builtin_trap();

    /* Invariant: all 32 output bytes should never be zero for any non-empty input
     * (extremely improbable for SHA-256). An all-zero output would indicate
     * a state initialization bug. */
    if (size > 0U) {
        uint8_t zero_check = 0U;
        for (int i = 0; i < 32; i++) zero_check |= digest[i];
        if (zero_check == 0U) __builtin_trap();
    }

    return 0;
}

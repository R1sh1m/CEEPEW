/* components/crypto/crypto_bench.h
 *
 * Same-board cryptographic baseline harness (campaign X2).
 *
 * The manuscript compares Ascon-128 v1.2 with alternatives on the SAME
 * board, at the SAME clock, with the SAME plaintext, so that the comparison
 * measures the primitive rather than the platform. This module produces one
 * machine-readable record per case:
 *
 *   BENCHCASE: name=ascon128_encrypt hw_aes=0 opt=O2 iters=30 \
 *              median_us=... min_us=... max_us=... mean_us=... \
 *              median_cycles=... cpu_hz=160000000
 *
 * The CPU frequency is read from the chip at run time and stored in every
 * record, so a run at an unexpected frequency is self-documenting instead
 * of silently skewed.
 *
 * NOTE ON MEMORY: PSA key import allocates inside the PSA implementation
 * (mbedTLS internal allocator). This is the only heap use in the module
 * and it is released with psa_destroy_key() before the module returns.
 * All CEE-PEW application buffers are static.
 */

#ifndef CRYPTO_BENCH_H
#define CRYPTO_BENCH_H

#include "ceepew_assert.h"
#include <stdint.h>
#include <stdbool.h>

#define CRYPTO_BENCH_MAX_CASES   16U
#define CRYPTO_BENCH_MAX_ITERS   256U

typedef struct {
    const char *name;        /* stable case identifier                      */
    bool        available;   /* false when the primitive is not in this build */
    bool        hw_aes;      /* ESP32 hardware AES accelerator is in the path */
    uint32_t    iters;       /* measured iterations                         */
    uint32_t    median_us;
    uint32_t    min_us;
    uint32_t    max_us;
    uint32_t    mean_us;
    uint64_t    median_cycles;
    uint32_t    cpu_hz;
} crypto_bench_case_t;

typedef struct {
    crypto_bench_case_t cases[CRYPTO_BENCH_MAX_CASES];
    uint32_t            case_count;
    uint32_t            cpu_hz;
    uint32_t            iterations;
    uint32_t            plaintext_len;
} crypto_bench_result_t;

/* Run every available case and log one BENCHCASE line per case.
 * iterations must be in [1, CRYPTO_BENCH_MAX_ITERS]. */
CeePewErr_t crypto_bench_run(uint32_t iterations, crypto_bench_result_t *out);

/* Log an already computed result set in the same machine-readable format. */
void crypto_bench_log_result(const crypto_bench_result_t *result);

#endif /* CRYPTO_BENCH_H */

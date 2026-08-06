/* tests/host/fuzz_driver.c - deterministic libFuzzer-style smoke driver
 *
 * Runs a fuzz harness's LLVMFuzzerTestOneInput() over its seed corpus
 * and a fixed PRNG mutation stream. Used when a native clang with
 * -fsanitize=fuzzer is not available (e.g. Windows hosts without
 * libFuzzer runtime).
 *
 * Each harness traps (__builtin_trap) on invariant violations, which
 * aborts the process with a non-zero exit code. With ASan/UBSan enabled
 * (non-MinGW toolchains) memory errors are also caught.
 *
 * Usage: fuzz_driver <seed_file>...
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_INPUT_BYTES 512U
#define MUTATION_ROUNDS 20000U

static uint32_t s_rng_state = 0x6D2B79F5U;

static uint32_t next_rand(void)
{
    uint32_t x = s_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng_state = x;
    return x;
}

int main(int argc, char **argv)
{
    static uint8_t buf[MAX_INPUT_BYTES];
    unsigned long iterations = 0UL;

    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (f == NULL) {
            fprintf(stderr, "[fuzz-driver] cannot open seed: %s\n", argv[i]);
            return 2;
        }
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        (void)LLVMFuzzerTestOneInput(buf, n);
        iterations++;
    }

    for (unsigned int round = 0U; round < MUTATION_ROUNDS; round++) {
        size_t len = (size_t)(next_rand() % (MAX_INPUT_BYTES + 1U));
        for (size_t j = 0U; j < len; j++) {
            buf[j] = (uint8_t)next_rand();
        }
        (void)LLVMFuzzerTestOneInput(buf, len);
        iterations++;
    }

    printf("[fuzz-driver] %lu inputs processed, no invariant violations\n",
           iterations);
    return 0;
}

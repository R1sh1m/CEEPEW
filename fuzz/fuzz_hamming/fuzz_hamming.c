/* fuzz/fuzz_hamming/fuzz_hamming.c — Hamming(15,11) FEC fuzz target
 *
 * Fuzzes ecc_hamming_encode() and ecc_hamming_decode().
 * Invariant: decode(encode(plaintext)) == plaintext for any input.
 * Also tests error-correction by flipping bits in the codeword.
 *
 * Build (libFuzzer):
 *   clang -fsanitize=fuzzer -I../../components/ecc -I../../components/ceepew_common \
 *         -I../../components/hal -I../../components/crypto \
 *         -c ../../components/ecc/ecc_hamming.c -o ecc_hamming.o
 *   clang -fsanitize=fuzzer -I../../components/ecc -I../../components/ceepew_common \
 *         -I../../components/hal -I../../components/crypto \
 *         fuzz_hamming.c ecc_hamming.o -o fuzz_hamming
 *
 * Build (AFL):
 *   afl-clang-fast ... (same flags, link with __AFL_FUZZ_INIT)
 */

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "ceepew_assert.h"

CeePewErr_t ecc_hamming_init_session(const uint8_t seed[16]);
CeePewErr_t ecc_hamming_encode(const uint8_t *in, uint16_t in_len,
                               uint8_t *out, uint16_t *out_len);
CeePewErr_t ecc_hamming_decode(const uint8_t *in, uint16_t in_len,
                               uint8_t *out, uint16_t *out_len,
                               bool *corrected);
CeePewErr_t ecc_hamming_deinit(void);

#ifndef CEEPEW_OK
#define CEEPEW_OK 0
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Need at least 16 bytes for session seed + 11 bytes for data */
    if (size < 27U) return 0;

    /* First 16 bytes are the session seed (session key[0:15]) */
    const uint8_t *seed = data;
    uint16_t data_len   = (uint16_t)(size - 16U);

    /* Clamp to avoid huge stack allocations */
    if (data_len > 256U) data_len = 256U;

    const uint8_t *payload = data + 16;

    /* Initialize session permutation */
    CeePewErr_t err = ecc_hamming_init_session(seed);
    if (err != CEEPEW_OK) return 0;

    uint8_t encoded[512];
    uint16_t encoded_len = 0U;

    err = ecc_hamming_encode(payload, data_len, encoded, &encoded_len);
    if (err != CEEPEW_OK) { ecc_hamming_deinit(); return 0; }

    /* Decode without corruption */
    uint8_t decoded[512];
    uint16_t decoded_len = 0U;
    bool corrected = false;

    err = ecc_hamming_decode(encoded, encoded_len, decoded, &decoded_len, &corrected);
    if (err != CEEPEW_OK) { ecc_hamming_deinit(); __builtin_trap(); }

    /* Invariant: decode(encode(x)) == x */
    if (decoded_len != data_len) { ecc_hamming_deinit(); __builtin_trap(); }
    if (memcmp(decoded, payload, data_len) != 0) { ecc_hamming_deinit(); __builtin_trap(); }

    /* Re-encode and flip a single bit in the codeword, then decode again.
     * Hamming(15,11) can correct any single-bit error. */
    if (encoded_len > 0U) {
        uint8_t corrupted[512];
        memcpy(corrupted, encoded, encoded_len);

        uint16_t flip_byte = (uint16_t)(data[0] % encoded_len);
        uint8_t  flip_bit  = (uint8_t)(data[1] & 0x07U);
        corrupted[flip_byte] ^= (uint8_t)(1U << flip_bit);

        uint8_t decoded2[512];
        uint16_t decoded2_len = 0U;
        bool corrected2 = false;

        err = ecc_hamming_decode(corrupted, encoded_len,
                                 decoded2, &decoded2_len, &corrected2);
        if (err != CEEPEW_OK) { ecc_hamming_deinit(); __builtin_trap(); }

        /* After single-bit flip, decode must match original and corrected=true */
        if (decoded2_len != data_len) { ecc_hamming_deinit(); __builtin_trap(); }
        if (memcmp(decoded2, payload, data_len) != 0) { ecc_hamming_deinit(); __builtin_trap(); }
        if (!corrected2) { ecc_hamming_deinit(); __builtin_trap(); }
    }

    ecc_hamming_deinit();
    return 0;
}

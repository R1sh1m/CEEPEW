/* fuzz/fuzz_ascon/fuzz_ascon.c — Ascon-128 AEAD fuzz target
 *
 * Fuzzes both crypto_ascon_aead_encrypt() and crypto_ascon_aead_decrypt()
 * with arbitrary inputs. The fuzzer provides:
 *   [0..15]    key (16 bytes)
 *   [16..31]   nonce (16 bytes)
 *   [32..]     concatenated AD + plaintext
 *
 * Build (libFuzzer):
 *   clang -fsanitize=fuzzer -I../../components/crypto -I../../components/ceepew_common \
 *         -I../../components/hal -c ../../components/crypto/crypto_ascon.c \
 *         -o crypto_ascon.o
 *   clang -fsanitize=fuzzer -I../../components/crypto -I../../components/ceepew_common \
 *         -I../../components/hal fuzz_ascon.c crypto_ascon.o \
 *         -o fuzz_ascon
 *
 * Build (AFL):
 *   afl-clang-fast ... (same flags as above, link with AFL's __AFL_FUZZ_INIT)
 */

#include <stdint.h>
#include <string.h>
#include "ceepew_assert.h"
#include "crypto_ascon.h"

#ifndef CEEPEW_OK
#define CEEPEW_OK 0
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Need at least 32 bytes for key + nonce */
    if (size < 32U) return 0;

    const uint8_t *key   = data;
    const uint8_t *nonce = data + 16;
    size_t remaining     = size - 32U;

    /* Split remaining bytes: use first half as AD, second as plaintext.
     * Zero-length is valid for both. */
    size_t ad_len = remaining / 2U;
    size_t pt_len = remaining - ad_len;

    const uint8_t *ad = data + 32U;
    const uint8_t *pt = data + 32U + ad_len;

    /* Encrypt: ct buffer needs pt_len + 16 tag bytes */
    uint8_t ct[512 + 16];
    uint16_t ct_len = 0U;
    if (pt_len > sizeof(ct) - 16U) return 0;  /* skip oversized */

    CeePewErr_t err = crypto_ascon_aead_encrypt(
        key, nonce, ad, (uint16_t)ad_len, pt, (uint16_t)pt_len,
        ct, &ct_len);

    if (err != CEEPEW_OK) return 0;

    /* Round-trip decrypt */
    uint8_t pt2[512];
    uint16_t pt2_len = 0U;
    err = crypto_ascon_aead_decrypt(
        key, nonce, ad, (uint16_t)ad_len, ct, ct_len,
        pt2, &pt2_len);

    /* Must succeed — encrypt-then-decrypt of our own data */
    if (err != CEEPEW_OK) __builtin_trap();

    return 0;
}

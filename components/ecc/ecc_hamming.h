/* components/ecc/ecc_hamming.h */

#ifndef ECC_HAMMING_H
#define ECC_HAMMING_H

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <stdint.h>
#include <stdbool.h>

/* Hamming (15,11) session-permuted FEC
 * Encodes 11 data bits -> 15 code bits. Blocks are processed from a bit-stream.
 */

/* Initialize session permutation using a 16-byte seed (session key[0:15]) */
CeePewErr_t ecc_hamming_init_session(const uint8_t seed[CEEPEW_SESSION_KEY_BYTES]);

/* Encode input bytes into FEC codewords. out_len is bytes produced. */
CeePewErr_t ecc_hamming_encode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len);

/* Decode input FEC codewords into original bytes. corrected is set true if
 * any single-bit errors were corrected. */
CeePewErr_t ecc_hamming_decode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len, bool *corrected);

#endif /* ECC_HAMMING_H */

/* components/crypto/crypto_hkdf.h */

#ifndef CRYPTO_HKDF_H
#define CRYPTO_HKDF_H

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <stdint.h>

CeePewErr_t crypto_hkdf_derive(const uint8_t *ikm, uint8_t ikm_len, const uint8_t *salt, uint8_t salt_len, const uint8_t *info, uint8_t info_len, uint8_t *out, uint8_t out_len);

/* Build canonical HKDF info field for session derivation.
 * out_info must be large enough for label + 6 + 6 + 32 + 4 bytes (>= 50).
 * out_info_max_len is the allocated size of out_info (bounds check).
 * out_len will be set to the number of bytes written.
 */
CeePewErr_t crypto_hkdf_build_info(const uint8_t *label, uint8_t label_len,
                                   const uint8_t id_a[6], const uint8_t id_b[6],
                                   const uint8_t commitment[32], uint32_t t_round,
                                   uint8_t *out_info, uint8_t out_info_max_len,
                                   uint8_t *out_len);

/* HKDF-Expand only (using a pre-computed PRK).
 * prk: 32-byte pseudorandom key (output of HKDF-Extract)
 * info: context info string
 * info_len: length of info
 * out: output buffer
 * out_len: desired output length (<= 64) */
CeePewErr_t crypto_hkdf_expand(const uint8_t *prk, const uint8_t *info, uint8_t info_len, uint8_t *out, uint8_t out_len);

#endif /* CRYPTO_HKDF_H */

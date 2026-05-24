#ifndef CRYPTO_EDDSA_H
#define CRYPTO_EDDSA_H

#include "ceepew_assert.h"
#include <stdint.h>

CeePewErr_t crypto_eddsa_keypair(uint8_t pk[32], uint8_t sk[64]);
CeePewErr_t crypto_eddsa_seeded_keypair(uint8_t pk[32], uint8_t sk[64], const uint8_t seed[32]);
CeePewErr_t crypto_eddsa_sign(const uint8_t priv[64], const uint8_t *msg, uint16_t msg_len, uint8_t sig[64]);
CeePewErr_t crypto_eddsa_verify(const uint8_t pub[32], const uint8_t *msg, uint16_t msg_len, const uint8_t sig[64]);

#endif /* CRYPTO_EDDSA_H */

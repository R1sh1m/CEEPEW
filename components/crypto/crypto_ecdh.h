/* components/crypto/crypto_ecdh.h */

#ifndef CRYPTO_ECDH_H
#define CRYPTO_ECDH_H

#include "ceepew_assert.h"
#include <stdint.h>

/* Generate ephemeral X25519 keypair from CSPRNG. */
CeePewErr_t crypto_ecdh_generate_keypair(uint8_t pub[32], uint8_t priv[32]);

/* Compute X25519 ECDH shared secret: ss = priv * pub. */
CeePewErr_t crypto_ecdh_shared_secret(const uint8_t priv[32],
                                      const uint8_t pub[32],
                                      uint8_t ss[32]);

#endif /* CRYPTO_ECDH_H */
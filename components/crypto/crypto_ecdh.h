/* components/crypto/crypto_ecdh.h */

#ifndef CRYPTO_ECDH_H
#define CRYPTO_ECDH_H

#include "ceepew_assert.h"
#include <stdint.h>

/* Generate an ephemeral X25519 keypair from CSPRNG.
 *
 * PARAMETERS:
 *   pub:  Output buffer for 32-byte public key (not NULL)
 *   priv: Output buffer for 32-byte private key (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Keypair generated
 *   CEEPEW_ERR_NULL_PTR — pub or priv is NULL
 *   CEEPEW_ERR_CRYPTO — Scalar base multiplication failed
 */
CeePewErr_t crypto_ecdh_generate_keypair(uint8_t pub[32], uint8_t priv[32]);

/* Compute X25519 ECDH shared secret: ss = priv * pub.
 *
 * PARAMETERS:
 *   priv: 32-byte private key (clamped internally)
 *   pub:  32-byte peer's public key
 *   ss:   Output buffer for 32-byte shared secret (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Shared secret computed
 *   CEEPEW_ERR_NULL_PTR — priv, pub, or ss is NULL
 *   CEEPEW_ERR_CRYPTO — Scalar multiplication failed
 */
CeePewErr_t crypto_ecdh_shared_secret(const uint8_t priv[32],
                                       const uint8_t pub[32],
                                       uint8_t ss[32]);

#endif /* CRYPTO_ECDH_H */

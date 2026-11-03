/* components/crypto/crypto_pake.h
 *
 * CEE-PEW Password-Authenticated Key Exchange (PAKE) Engine.
 *
 * Implements a balanced PAKE over Curve25519 (Montgomery form) based on RFC 9382
 * (CPace) principles, eliminating the offline dictionary attack on short pairing
 * codes (e.g. 4-character codes with 36^4 = 1,679,616 combinations).
 *
 * SECURITY INVARIANTS:
 *   1. Zero Heap: All operations execute within caller stack buffers. No malloc/free.
 *   2. Offline Attack Immunity: Passive eavesdroppers observing public exchanges
 *      (A, B, confirmation tags) cannot verify a password guess without solving
 *      Decisional Diffie-Hellman in the prime-order subgroup.
 *   3. Ephemeral Erasure: Private scalars and intermediate point multiplications
 *      are securely zeroed via ceepew_secure_zero() prior to function return.
 *   4. Low-Order Immunity: Eliminates small-subgroup points by rejecting non-canonical
 *      and low-order points, and clearing the cofactor 8.
 */

#ifndef CEEPEW_CRYPTO_PAKE_H
#define CEEPEW_CRYPTO_PAKE_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CEEPEW_PAKE_SALT_BYTES       16U
#define CEEPEW_PAKE_PUBKEY_BYTES     32U
#define CEEPEW_PAKE_CONFIRM_BYTES    16U
#define CEEPEW_PAKE_KEY_BYTES        32U

/* Initialize PAKE exchange:
 * 1. Derives base point G from the password and salt via HKDF-SHA256 and cofactor multiplication.
 * 2. Generates ephemeral scalar 'local_priv' and computes 'local_pub' = local_priv * G.
 * Returns CEEPEW_OK on success.
 */
CeePewErr_t crypto_pake_init(const char *code, uint16_t code_len,
                             const uint8_t salt[CEEPEW_PAKE_SALT_BYTES],
                             uint8_t local_priv[32U],
                             uint8_t local_pub[CEEPEW_PAKE_PUBKEY_BYTES]);

/* Compute shared secret and confirmation tags:
 * 1. Checks peer_pub against low-order and non-canonical representations.
 * 2. Computes K = local_priv * peer_pub = local_priv * peer_priv * G.
 * 3. Derives shared session key (32 B), local confirmation tag (16 B),
 *    and expected peer confirmation tag (16 B) binding transcript bytes.
 * 4. Securely zeroes intermediate scalar multiplication buffers.
 */
CeePewErr_t crypto_pake_compute(const uint8_t local_priv[32U],
                                const uint8_t peer_pub[CEEPEW_PAKE_PUBKEY_BYTES],
                                const uint8_t *transcript, uint16_t transcript_len,
                                bool is_initiator,
                                uint8_t session_key_out[CEEPEW_PAKE_KEY_BYTES],
                                uint8_t local_confirm_out[CEEPEW_PAKE_CONFIRM_BYTES],
                                uint8_t peer_confirm_expected[CEEPEW_PAKE_CONFIRM_BYTES]);

/* Constant-time verification of peer confirmation tag.
 * Returns CEEPEW_OK if match, CEEPEW_ERR_CRYPTO if mismatch.
 */
CeePewErr_t crypto_pake_verify_confirm(const uint8_t received_confirm[CEEPEW_PAKE_CONFIRM_BYTES],
                                       const uint8_t expected_confirm[CEEPEW_PAKE_CONFIRM_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* CEEPEW_CRYPTO_PAKE_H */


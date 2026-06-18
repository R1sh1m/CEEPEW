/* components/transport/transport_ble_gatt_crypto.h
 *
 * Encrypted GATT sign_pk handoff (Phase 7 — Hybrid-GATT).
 *
 * The BLE GATTS write of the initiator's Ed25519 sign_pk is wrapped in
 * Ascon-128 AEAD using a key/nonce derived from the human-verified
 * session_code. Because the session code is the trust anchor of the
 * pairing flow (both peers must enter the same value), the derived key
 * is identical at both ends without any key exchange — only peers that
 * already share the session code can decrypt the GATT payload.
 *
 * Key derivation:
 *   salt  = SHA256("CEEPEW_GATT_SALT_v1")
 *   info  = "CEEPEW_GATT_SIGN_PK_v1" || idA || idB
 *          (idA/idB sorted lexicographically so both peers agree)
 *   key   = HKDF-SHA256(ikm=session_code, salt, info)[:16]
 *
 * Nonce derivation:
 *   nonce = SHA256("CEEPEW_GATT_NONCE_v1" || session_code)[:16]
 *
 * The nonce is deterministic per session (both peers compute identically)
 * and is never transmitted on the wire. It is bound to the session_code
 * by inclusion in the SHA-256 input, so a captured ciphertext cannot be
 * replayed against a different session.
 *
 * Wire format carries sign_pk[32] || box_pubkey[32] || wifi_mac[6] = 70B plaintext,
 * producing 70B ciphertext + 16B tag = 86B on the wire.
 * (negotiated GATT MTU must be >= 90 to fit a single write —
 *  see ESP_GATTC_CFG_MTU_EVT in transport_ble.c.)
 */

#ifndef TRANSPORT_BLE_GATT_CRYPTO_H
#define TRANSPORT_BLE_GATT_CRYPTO_H

#include <stdint.h>
#include "ceepew_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Total length of the encrypted GATT payload (ct || tag).
 * Wire format carries sign_pk[32] || box_pubkey[32] || wifi_mac[6] = 70B plaintext,
 * producing 70B ciphertext + 16B tag = 86B on the wire. */
#define GATT_PLAINTEXT_BYTES    70U
#define GATT_CIPHERTEXT_BYTES   70U
#define GATT_TAG_BYTES          16U
#define GATT_CRYPTO_TOTAL_BYTES (GATT_CIPHERTEXT_BYTES + GATT_TAG_BYTES)

/* Ascon-128 key and nonce are both 16 bytes. */
#define GATT_KEY_BYTES         16U
#define GATT_NONCE_BYTES       16U

/* Encrypted 70-byte (sign_pk || box_pubkey || wifi_mac) under the session_code-derived key.
 * Output is 86 bytes (70B ct + 16B tag). Both peers use the same session_code
 * and sorted (id_self, id_peer) ordering, so the receiver can
 * independently derive the same key/nonce and decrypt.
 *
 * PARAMETERS:
 *   session_code: 32-byte human-verified session code (not NULL)
 *   id_self:      6-byte local MAC (not NULL)
 *   id_peer:      6-byte peer MAC (not NULL)
 *   plaintext:    70-byte sign_pk[32] || box_pubkey[32] || wifi_mac[6] (not NULL)
 *   out:          86-byte output buffer for ct+tag (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK
 *   CEEPEW_ERR_NULL_PTR — any param NULL
 *   CEEPEW_ERR_CRYPTO — key derivation or Ascon encrypt failed
 */
CeePewErr_t gatt_crypto_encrypt_with_ids(const uint8_t session_code[32],
                                          const uint8_t id_self[6],
                                          const uint8_t id_peer[6],
                                          const uint8_t plaintext[GATT_PLAINTEXT_BYTES],
                                          uint8_t out[GATT_CRYPTO_TOTAL_BYTES]);

/* Decrypt an 86-byte (ct || tag) GATT payload back to a 70-byte
 * (sign_pk || box_pubkey || wifi_mac). Authentication is mandatory: any tag
 * mismatch returns CEEPEW_ERR_AUTH_FAIL without revealing the plaintext.
 *
 * PARAMETERS:
 *   session_code: 32-byte session code (same value as used to encrypt)
 *   id_self:      6-byte local MAC (not NULL)
 *   id_peer:      6-byte peer's MAC (not NULL)
 *   in:           86-byte ct+tag buffer (not NULL)
 *   plaintext_out:70-byte output buffer for sign_pk[32] || box_pubkey[32] || wifi_mac[6]
 *
 * RETURNS:
 *   CEEPEW_OK — Plaintext recovered and authenticated
 *   CEEPEW_ERR_AUTH_FAIL — Tag mismatch (silent discard; caller should
 *                          bump reconnect_attempts and reset the link)
 *   CEEPEW_ERR_NULL_PTR
 *   CEEPEW_ERR_CRYPTO — key derivation or Ascon decrypt failed
 */
CeePewErr_t gatt_crypto_decrypt_with_ids(const uint8_t session_code[32],
                                          const uint8_t id_self[6],
                                          const uint8_t id_peer[6],
                                          const uint8_t in[GATT_CRYPTO_TOTAL_BYTES],
                                          uint8_t plaintext_out[GATT_PLAINTEXT_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_BLE_GATT_CRYPTO_H */

/* components/crypto/crypto_hmac_efuse.h */

#ifndef CRYPTO_HMAC_EFUSE_H
#define CRYPTO_HMAC_EFUSE_H

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <stdint.h>
#include <stdbool.h>

#define CEEPEW_EFUSE_IDENTITY_KEY_LEN  32U  /* Device identity key length   */

/* Check whether the user eFuse block has been provisioned with a device
 * identity key.  On ESP32, user data is stored in eFuse block 3 (256 bits).
 * We check for a non-zero first byte (statistically certain for random key). */
bool crypto_hmac_efuse_is_provisioned(void);

/* One-shot: generate a random 32-byte device identity key and write it to
 * the user eFuse block (EFUSE_BLK3).  Returns CEEPEW_OK on success,
 * CEEPEW_ERR_HW if eFuse operations fail, or CEEPEW_OK if already
 * provisioned (idempotent).
 *
 * WARNING: eFuse writes are one-way.  This function should only be called
 * once per device during factory provisioning or first boot. */
CeePewErr_t crypto_hmac_efuse_provision(void);

/* Read the device identity key from the eFuse user block.
 * The key must have been previously written by crypto_hmac_efuse_provision().
 * Returns CEEPEW_OK on success, CEEPEW_ERR_STATE if not provisioned. */
CeePewErr_t crypto_hmac_efuse_read_key(uint8_t key_out[32U]);

/* Derive the HKDF salt for session key derivation.
 *
 * Computes:
 *   salt = SHA-256( HMAC-SHA256(efuse_identity, ds_mix)
 *                  || SHA-256(ds_mix || code) )
 *
 * This adds a device-bound factor to the HKDF salt: even if two devices
 * share the same session code, the eFuse-derived portion differs per
 * device identity, preventing cross-device key prediction.
 *
 * If the eFuse block is not provisioned, falls back to:
 *   salt = SHA-256(ds_mix || code)
 * which is the original pre-eFuse behavior.
 *
 * Parameters:
 *   ds_mix      - 32-byte output of digital_sum_mix(code)
 *   code        - 32-byte session code
 *   hkdf_salt   - output buffer, must be >= 32 bytes
 *
 * Returns CEEPEW_OK on success, or error if crypto operations fail. */
CeePewErr_t crypto_hmac_efuse_derive_salt(const uint8_t ds_mix[32U],
                                           const uint8_t code[32U],
                                           uint8_t hkdf_salt[32U]);

#endif /* CRYPTO_HMAC_EFUSE_H */

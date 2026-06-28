/* components/crypto/crypto_hmac_efuse.h */

#ifndef CRYPTO_HMAC_EFUSE_H
#define CRYPTO_HMAC_EFUSE_H

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <stdint.h>
#include <stdbool.h>

#define CEEPEW_EFUSE_IDENTITY_KEY_LEN  32U

/* Check if eFuse user block (BLK3) has a provisioned identity key. */
bool crypto_hmac_efuse_is_provisioned(void);

/* Provision eFuse BLK3 with random 32-byte identity key. One-way write — call once.
 * Idempotent: returns OK if already provisioned. */
CeePewErr_t crypto_hmac_efuse_provision(void);

/* Read device identity key from eFuse. Requires prior provision(). */
CeePewErr_t crypto_hmac_efuse_read_key(uint8_t key_out[32U]);

/* Derive HKDF salt with device-bound factor:
 *   salt = SHA-256(HMAC(efuse, ds_mix) || SHA-256(ds_mix || code))
 * Falls back to SHA-256(ds_mix || code) if eFuse not provisioned. */
CeePewErr_t crypto_hmac_efuse_derive_salt(const uint8_t ds_mix[32U],
                                           const uint8_t code[32U],
                                           uint8_t hkdf_salt[32U]);

#endif /* CRYPTO_HMAC_EFUSE_H */

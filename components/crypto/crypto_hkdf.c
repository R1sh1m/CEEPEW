/* components/crypto/crypto_hkdf.c */

#include "crypto_hkdf.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include <stdint.h>

#include <mbedtls/md.h>
#include <mbedtls/hkdf.h>

/* SECURITY: The HKDF salt must be SHA256(digital_sum_mix(code) || code) as
 * per Final Spec §3.3. The session code is the only secret the HKDF caller
 * holds — without it, ECDH shared secret alone is insufficient to derive the
 * session key.
 */
CeePewErr_t crypto_hkdf_derive(const uint8_t *ikm, uint8_t ikm_len, const uint8_t *salt, uint8_t salt_len, const uint8_t *info, uint8_t info_len, uint8_t *out, uint8_t out_len){
    CEEPEW_ASSERT(ikm != NULL && ikm_len > 0U && ikm_len <= 32U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(salt != NULL || salt_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len > 0U && out_len <= 64U, CEEPEW_ERR_BOUNDS);

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL){ return CEEPEW_ERR_CRYPTO;}

    int rc = mbedtls_hkdf(md, salt, (size_t)salt_len, ikm, (size_t)ikm_len, info, (size_t)info_len, out, (size_t)out_len);
    CEEPEW_ASSERT(rc == 0, CEEPEW_ERR_CRYPTO);

    return CEEPEW_OK;
}

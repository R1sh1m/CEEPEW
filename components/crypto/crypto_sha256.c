/* components/crypto/crypto_sha256.c */

#include "crypto_sha256.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include <stdint.h>

#include "mbedtls/sha256.h"

CeePewErr_t crypto_sha256_compute(const uint8_t *in, uint32_t len, uint8_t out[32])
{
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U, CEEPEW_ERR_BOUNDS);

    /* Use mbedtls SHA256 API (returns 0 on success) */
    int rc = mbedtls_sha256_ret(in, (size_t)len, out, 0);
    CEEPEW_ASSERT(rc == 0, CEEPEW_ERR_CRYPTO);

    return CEEPEW_OK;
}

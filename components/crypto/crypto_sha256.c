/* components/crypto/crypto_sha256.c */

#include "crypto_sha256.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include <stdint.h>

#include <mbedtls/sha256.h>

CeePewErr_t crypto_sha256_compute(const uint8_t *in, uint32_t len, uint8_t out[32])
{
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U, CEEPEW_ERR_BOUNDS);

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);

    int rc = mbedtls_sha256_starts(&ctx, 0);
    if (rc != 0)
    {
        mbedtls_sha256_free(&ctx);
        return CEEPEW_ERR_CRYPTO;
    }
    rc = mbedtls_sha256_update(&ctx, in, (size_t)len);
    if (rc != 0)
    {
        mbedtls_sha256_free(&ctx);
        return CEEPEW_ERR_CRYPTO;
    }
    rc = mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
    CEEPEW_ASSERT(rc == 0, CEEPEW_ERR_CRYPTO);

    return CEEPEW_OK;
}

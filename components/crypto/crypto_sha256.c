/* components/crypto/crypto_sha256.c */

#include "crypto_sha256.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <stdint.h>

#include <mbedtls/md.h>

CeePewErr_t crypto_sha256_compute(const uint8_t *in, uint32_t len, uint8_t out[32]) {
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U, CEEPEW_ERR_BOUNDS);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) { return CEEPEW_ERR_CRYPTO; }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, info, 0);
    if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }

    rc = mbedtls_md_starts(&ctx);
    if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }

    rc = mbedtls_md_update(&ctx, in, (size_t)len);
    if (rc != 0) { mbedtls_md_free(&ctx); return CEEPEW_ERR_CRYPTO; }

    rc = mbedtls_md_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    if (rc != 0) { return CEEPEW_ERR_CRYPTO; }

    return CEEPEW_OK;
}

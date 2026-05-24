/* components/crypto/crypto_ctx.c */

#include "crypto_ctx.h"
#include "ceepew_security_utils.h"

#include <string.h>

CryptoCtx_t g_crypto_ctx = {0};

CeePewErr_t crypto_ctx_init(void)
{
    CEEPEW_ASSERT(sizeof(CryptoCtx_t) <= CEEPEW_REGION_POOL_BYTES,
                  CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(&g_crypto_ctx != NULL, CEEPEW_ERR_INTERNAL);

    memset(&g_crypto_ctx, 0, sizeof(g_crypto_ctx));
    g_crypto_ctx.session_active = false;
    return CEEPEW_OK;
}

CeePewErr_t crypto_ctx_destroy(void)
{
    CEEPEW_ASSERT(sizeof(CryptoCtx_t) <= CEEPEW_REGION_POOL_BYTES,
                  CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(&g_crypto_ctx != NULL, CEEPEW_ERR_INTERNAL);

    ceepew_secure_zero(&g_crypto_ctx, (uint32_t)sizeof(g_crypto_ctx));
    return CEEPEW_OK;
}

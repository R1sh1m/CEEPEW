/* components/crypto/crypto_ctx.c */

#include "crypto_ctx.h"
#include "ceepew_security_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

CryptoCtx_t g_crypto_ctx = {0};
SemaphoreHandle_t g_crypto_mutex = NULL;

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
    
    /* Destroy mutex if it exists */
    if (g_crypto_mutex != NULL) {
        vSemaphoreDelete(g_crypto_mutex);
        g_crypto_mutex = NULL;
    }
    return CEEPEW_OK;
}

CeePewErr_t crypto_mutex_init(void)
{
    CEEPEW_ASSERT(g_crypto_mutex == NULL, CEEPEW_ERR_PARAM);
    
    g_crypto_mutex = xSemaphoreCreateMutex();
    if (g_crypto_mutex == NULL) {
        return CEEPEW_ERR_PARAM;
    }
    return CEEPEW_OK;
}

CeePewErr_t crypto_mutex_lock(void)
{
    CEEPEW_ASSERT(g_crypto_mutex != NULL, CEEPEW_ERR_PARAM);
    
    if (xSemaphoreTake(g_crypto_mutex, portMAX_DELAY) != pdTRUE) {
        return CEEPEW_ERR_PARAM;
    }
    return CEEPEW_OK;
}

CeePewErr_t crypto_mutex_unlock(void)
{
    CEEPEW_ASSERT(g_crypto_mutex != NULL, CEEPEW_ERR_PARAM);
    
    if (xSemaphoreGive(g_crypto_mutex) != pdTRUE) {
        return CEEPEW_ERR_PARAM;
    }
    return CEEPEW_OK;
}

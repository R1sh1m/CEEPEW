/* components/crypto/crypto_ctx.c */

#include "crypto_ctx.h"
#include "crypto_sha256.h"
#include "ceepew_security_utils.h"
#include "ceepew_assert.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdbool.h>
#include "psa/crypto.h"

CryptoCtx_t g_crypto_ctx = {0};
SemaphoreHandle_t g_crypto_mutex = NULL;

static bool s_sha256_warmed_up = false;

CeePewErr_t crypto_ctx_init(void)
{
    CEEPEW_ASSERT(sizeof(CryptoCtx_t) <= CEEPEW_REGION_POOL_BYTES,
                  CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(&g_crypto_ctx != NULL, CEEPEW_ERR_INTERNAL);

    memset(&g_crypto_ctx, 0, sizeof(g_crypto_ctx));
    g_crypto_ctx.session_active = false;

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return CEEPEW_ERR_CRYPTO;
    }

    /* One-time SHA256 warm-up: on ESP32 the first call to
     * mbedtls_md_setup / mbedtls_md_starts after psa_crypto_init()
     * can fail if the PSA entropy accumulator or driver hasn't fully
     * settled. A single successful hash forces all lazy init to
     * complete so that subsequent calls succeed immediately. */
    if (!s_sha256_warmed_up) {
        static const uint8_t warmup_msg[1U] = { 0x00U };
        uint8_t warmup_hash[32U];
        CeePewErr_t werr = crypto_sha256_compute(warmup_msg, 1U, warmup_hash);
        if (werr != CEEPEW_OK) {
            return werr;
        }
        /* Secure zero the warm-up output (contains no secrets, but
         * follow the coding convention regardless). */
        volatile uint8_t *p = warmup_hash;
        for (uint8_t i = 0U; i < 32U; i++) { p[i] = 0U; }
        s_sha256_warmed_up = true;
    }

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
    if (g_crypto_mutex != NULL) {
        return CEEPEW_OK;
    }

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

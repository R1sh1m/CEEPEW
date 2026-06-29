/* main/ceepew_assert.c
 *
 * Implementation of ceepew_log_assert, the logger called by every
 * CEEPEW_ASSERT macro site. Hosted in main/ so all components can
 * link against it without depending on the OLED driver.
 *
 * CEEPEW_DEBUG_SERIAL is defined globally via -D in the project's
 * CMake toolchain file (and gated by main/ceepew_config.h). We use
 * #ifdef here to avoid a circular REQUIRES with main.
 */
#include "ceepew_assert.h"
#include <esp_log.h>

static const char *TAG = "CEEPEW";

static const char *ceepew_err_to_str(CeePewErr_t code){
    switch (code){
        case CEEPEW_OK: return "CEEPEW_OK";
        case CEEPEW_ERR_NULL_PTR: return "CEEPEW_ERR_NULL_PTR";
        case CEEPEW_ERR_BOUNDS: return "CEEPEW_ERR_BOUNDS";
        case CEEPEW_ERR_PARAM: return "CEEPEW_ERR_PARAM";
        case CEEPEW_ERR_NONCE_EXHAUSTED: return "CEEPEW_ERR_NONCE_EXHAUSTED";
        case CEEPEW_ERR_CRYPTO: return "CEEPEW_ERR_CRYPTO";
        case CEEPEW_ERR_TRANSPORT: return "CEEPEW_ERR_TRANSPORT";
        case CEEPEW_ERR_FEC: return "CEEPEW_ERR_FEC";
        case CEEPEW_ERR_PINS: return "CEEPEW_ERR_PINS";
        case CEEPEW_ERR_ALLOC: return "CEEPEW_ERR_ALLOC";
        case CEEPEW_ERR_INTERNAL: return "CEEPEW_ERR_INTERNAL";
        case CEEPEW_ERR_TIMEOUT: return "CEEPEW_ERR_TIMEOUT";
        case CEEPEW_ERR_OVERFLOW: return "CEEPEW_ERR_OVERFLOW";
        case CEEPEW_ERR_UNSUPPORTED: return "CEEPEW_ERR_UNSUPPORTED";
        case CEEPEW_ERR_BUSY: return "CEEPEW_ERR_BUSY";
        case CEEPEW_ERR_HW: return "CEEPEW_ERR_HW";
        case CEEPEW_ERR_NOENT: return "CEEPEW_ERR_NOENT";
        case CEEPEW_ERR_REPLAY: return "CEEPEW_ERR_REPLAY";
        case CEEPEW_ERR_SIG_FAIL: return "CEEPEW_ERR_SIG_FAIL";
        case CEEPEW_ERR_MAX_RETRIES: return "CEEPEW_ERR_MAX_RETRIES";
        case CEEPEW_ERR_AUTH_FAIL: return "CEEPEW_ERR_AUTH_FAIL";
        case CEEPEW_ERR_FEC_UNCORRECT: return "CEEPEW_ERR_FEC_UNCORRECT";
        default: return "CEEPEW_ERR_UNKNOWN";
    }
}

void ceepew_log_assert(const char *expr, const char *file, int line, CeePewErr_t code){
    if (file == NULL || expr == NULL) { return; }

    ESP_LOGE(TAG, "[CEEPEW ASSERT] %s:%d - %s (err=%s:%d)", file, line, expr, ceepew_err_to_str(code), (int)code);
}

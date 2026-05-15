/* main/ceepew_assert.c */
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <esp_log.h>

static const char *TAG = "CEEPEW";

void ceepew_log_assert(const char *expr, const char *file, int line, CeePewErr_t code)
{
    /* Two guard assertions as required */
    CEEPEW_ASSERT_VOID(file != NULL);
    CEEPEW_ASSERT_VOID(expr != NULL);

    /* Log using ESP_LOGE with the exact required format */
    ESP_LOGE(TAG, "[CEEPEW ASSERT] %s:%d — %s (err=%d)",
             file, line, expr, (int)code);

#ifdef CEEPEW_DEBUG_SERIAL
    /* Duplicate to serial logger when debugging enabled */
    ESP_LOGE(TAG, "[CEEPEW ASSERT] %s:%d — %s (err=%d)",
             file, line, expr, (int)code);
#endif
}

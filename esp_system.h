/* esp_system.h - root-level compatibility declarations */

#ifndef ESP_SYSTEM_H_ROOT
#define ESP_SYSTEM_H_ROOT

#include <stddef.h>
#include <stdint.h>

/* Provided by ESP-IDF (esp_random component) at link time. */
uint32_t esp_random(void);

/* ESP-IDF-compatible random buffer fill using esp_random() words. */
static inline void esp_fill_random(void *buf, size_t len)
{
    uint8_t *out = (uint8_t *)buf;
    size_t offset = 0U;

    if (out == NULL || len == 0U) {
        return;
    }

    while (offset < len) {
        uint32_t rnd = esp_random();
        size_t remain = len - offset;
        size_t copy_len = (remain < 4U) ? remain : 4U;
        for (size_t i = 0U; i < copy_len; i++) {
            out[offset + i] = (uint8_t)((rnd >> (8U * (uint32_t)i)) & 0xFFU);
        }
        offset += copy_len;
    }
}

#endif /* ESP_SYSTEM_H_ROOT */

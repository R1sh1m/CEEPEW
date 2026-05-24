/* esp_crc.h - root-level compatibility implementation */

#ifndef ESP_CRC_H_ROOT
#define ESP_CRC_H_ROOT

#include <stddef.h>
#include <stdint.h>

/* CRC-32/IEEE (reversed polynomial 0xEDB88320), little-endian bit order.
   Matches common ESP-IDF usage with chaining through the crc parameter. */
static inline uint32_t esp_crc32_le(uint32_t crc, const void *buf, size_t size)
{
    const uint8_t *data = (const uint8_t *)buf;
    uint32_t c = ~crc;

    if (data == NULL && size > 0U) {
        return 0U;
    }

    for (size_t idx = 0U; idx < size; idx++) {
        c ^= (uint32_t)data[idx];
        for (uint8_t bit = 0U; bit < 8U; bit++) {
            uint32_t mask = 0U - (c & 1U);
            c = (c >> 1U) ^ (0xEDB88320U & mask);
        }
    }

    return ~c;
}

#endif /* ESP_CRC_H_ROOT */

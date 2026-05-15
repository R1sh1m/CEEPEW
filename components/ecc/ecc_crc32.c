/* components/ecc/ecc_crc32.c */

#include "ecc_crc32.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include <stdint.h>
#include <string.h>

/* ESP-IDF CRC API */
#include "esp_crc.h"

CeePewErr_t ecc_crc32_compute(const uint8_t *data, uint16_t len, uint32_t *crc_out)
{
    CEEPEW_ASSERT(data != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(crc_out != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);

    uint32_t crc = esp_crc32_le(0U, data, (size_t)len);
    *crc_out = crc;
    return CEEPEW_OK;
}

CeePewErr_t ecc_crc32_verify(const uint8_t *frame, uint16_t frame_len)
{
    CEEPEW_ASSERT(frame != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(frame_len >= 4U, CEEPEW_ERR_BOUNDS);

    uint16_t data_len = (uint16_t)(frame_len - 4U);
    uint32_t expected = esp_crc32_le(0U, frame, (size_t)data_len);

    /* Extract appended CRC (little-endian) */
    uint32_t appended = ((uint32_t)frame[data_len + 0U])
                      | ((uint32_t)frame[data_len + 1U] << 8U)
                      | ((uint32_t)frame[data_len + 2U] << 16U)
                      | ((uint32_t)frame[data_len + 3U] << 24U);

    if (expected == appended) {
        return CEEPEW_OK;
    }

    return CEEPEW_ERR_FEC;
}

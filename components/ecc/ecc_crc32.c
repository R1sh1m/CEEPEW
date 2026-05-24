/* components/ecc/ecc_crc32.c */

#include "ecc_crc32.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "esp_crc.h"
#include <stddef.h>
#include <stdint.h>

static uint32_t load_u32_le(const uint8_t *src) {
    return ((uint32_t)src[0]) | ((uint32_t)src[1] << 8U) | ((uint32_t)src[2] << 16U) | ((uint32_t)src[3] << 24U);
}

CeePewErr_t ecc_crc32_compute(const uint8_t *data, uint16_t len, uint32_t *crc_out) {
    CEEPEW_ASSERT(data != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(crc_out != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(len <= CEEPEW_PACKET_MAX_BYTES, CEEPEW_ERR_BOUNDS);
    *crc_out = esp_crc32_le(0U, data, (size_t)len);
    return CEEPEW_OK;
}

CeePewErr_t ecc_crc32_verify(const uint8_t *frame, uint16_t frame_len) {
    CEEPEW_ASSERT(frame != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(frame_len > sizeof(uint32_t), CEEPEW_ERR_BOUNDS);
    uint16_t payload_len = (uint16_t)(frame_len - (uint16_t)sizeof(uint32_t));
    CEEPEW_ASSERT(payload_len <= CEEPEW_PACKET_MAX_BYTES, CEEPEW_ERR_BOUNDS);
    uint32_t expected_crc = esp_crc32_le(0U, frame, (size_t)payload_len);
    uint32_t received_crc = load_u32_le(&frame[payload_len]);

    if (expected_crc != received_crc){ return CEEPEW_ERR_FEC; }
    return CEEPEW_OK;
}

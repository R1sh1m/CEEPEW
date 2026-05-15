/* components/ecc/ecc_crc32.h */

#ifndef ECC_CRC32_H
#define ECC_CRC32_H

#include "../../main/ceepew_config.h"
#include "../../main/ceepew_assert.h"
#include <stdint.h>

CeePewErr_t ecc_crc32_compute(const uint8_t *data, uint16_t len, uint32_t *crc_out);
CeePewErr_t ecc_crc32_verify(const uint8_t *frame, uint16_t frame_len);

#endif /* ECC_CRC32_H */

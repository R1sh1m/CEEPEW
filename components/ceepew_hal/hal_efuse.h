/* components/ceepew_hal/hal_efuse.h
 *
 * CEE-PEW eFuse Interface for Device Secret Storage
 *
 * The ESP32 contains OTP eFuse blocks that persist across power cycles
 * and are hardware-protected. This module reads the device-unique secret
 * stored in eFuse, which is bound to each hardware unit.
 *
 * The device secret is used in Ed25519 keypair derivation to ensure
 * that signing keys are unique per device and cannot be predicted
 * from publicly-known session parameters.
 */

#ifndef HAL_EFUSE_H
#define HAL_EFUSE_H

#include "ceepew_assert.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read 32-byte device secret from eFuse storage.
 * This secret is unique per ESP32 unit and persists across sessions.
 * On first manufacture, this should be provisioned with high-entropy data.
 *
 * If eFuse is not provisioned (all zeros), this function returns
 * CEEPEW_ERR_HW. The session layer must handle this gracefully.
 *
 * PARAMETERS:
 *   secret_out: Output buffer for 32-byte secret (not NULL)
 *
 * RETURNS:
 *   CEEPEW_OK — Secret read successfully
 *   CEEPEW_ERR_HW — eFuse read failed or not provisioned
 *   CEEPEW_ERR_NULL_PTR — secret_out is NULL
 */
CeePewErr_t hal_efuse_get_device_secret(uint8_t secret_out[32]);

#ifdef __cplusplus
}
#endif

#endif /* HAL_EFUSE_H */

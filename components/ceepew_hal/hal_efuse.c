/* components/ceepew_hal/hal_efuse.c
 *
 * Device identity: tries eFuse BLOCK_KEY0 first, falls back to MAC-derived.
 *
 * M8: Production devices should have a unique 256-bit key burned into
 * EFUSE_BLK_KEY0 during manufacturing. Development boards start with
 * an empty block, so the MAC-derived fallback keeps things working.
 */

#include "hal_efuse.h"
#include "ceepew_security_utils.h"
#include "ceepew_assert.h"
#include "esp_efuse.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "../crypto/crypto_sha256.h"

#include <string.h>

CeePewErr_t hal_efuse_get_device_secret(uint8_t secret_out[32])
{
    CEEPEW_ASSERT(secret_out != NULL, CEEPEW_ERR_NULL_PTR);

#ifdef SOC_EFUSE_KEY_PURPOSE_FIELD
    /* Try reading a manufacturer-provisioned key from BLOCK_KEY0.
     * esp_efuse_key_block_unused() returns true if the block has
     * never been written (all zeros). If it's written, we read
     * the raw 256-bit value directly. */
    if (!esp_efuse_key_block_unused(ESP_EFUSE_KEY_0)) {
        uint8_t raw_key[32];
        esp_err_t err = esp_efuse_read_block(ESP_EFUSE_KEY_0,
                                              raw_key, 0U, 256U);
        if (err == ESP_OK) {
            /* Verify key is not all-zeros (paranoia — should never
             * happen if key_block_unused returned false) */
            uint8_t nonzero = 0U;
            for (uint16_t i = 0U; i < 32U; i++) {
                nonzero |= raw_key[i];
            }
            if (nonzero != 0U) {
                memcpy(secret_out, raw_key, 32U);
                ceepew_secure_zero(raw_key, sizeof(raw_key));
                return CEEPEW_OK;
            }
        }
        ceepew_secure_zero(raw_key, sizeof(raw_key));
        /* Fall through to MAC-derived if eFuse read failed or was zero */
    }
#endif

    /* Fallback: Derive a device secret from the MAC address.
     *
     * The result is: SHA256(MAC_address || "CEEPEW_DEVICE_SECRET_v1")
     *
     * For real security, burn a unique key into BLOCK_KEY0 during
     * manufacturing and leave the fallback unused. */
    uint8_t mac_addr[6];
    esp_err_t err = esp_efuse_mac_get_default(mac_addr);
    if (err != ESP_OK) {
        return CEEPEW_ERR_HW;
    }

    uint8_t seed_buf[6 + 20];
    memcpy(&seed_buf[0], mac_addr, 6);
    const char *salt = "CEEPEW_DEVICE_SECRET_v1";
    memcpy(&seed_buf[6], salt, 20);

    CeePewErr_t ceepew_err = crypto_sha256_compute(seed_buf, sizeof(seed_buf), secret_out);
    ceepew_secure_zero(seed_buf, sizeof(seed_buf));

    return ceepew_err;
}

/* components/ceepew_hal/hal_efuse.c */

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

    /* Fallback implementation: Derive a device secret from the MAC address.
     * 
     * On production systems, a manufacturer-provisioned 256-bit eFuse field
     * should be used instead. This implementation uses MAC address which is
     * unique per ESP32 device, then expands it to 32 bytes via SHA256.
     * 
     * The result is: SHA256(MAC_address || "CEEPEW_DEVICE_SECRET_v1")
     *
     * For real security, use a proper hardware-backed provisioning system.
     */
    
    uint8_t mac_addr[6];
    esp_err_t err = esp_efuse_mac_get_default(mac_addr);
    if (err != ESP_OK) {
        return CEEPEW_ERR_HW;  /* Cannot read MAC */
    }

    /* Combine MAC with a constant salt to create seed material */
    uint8_t seed_buf[6 + 20];
    memcpy(&seed_buf[0], mac_addr, 6);
    /* Use a known string to ensure this is a CEE-PEW device */
    const char *salt = "CEEPEW_DEVICE_SECRET_v1";
    memcpy(&seed_buf[6], salt, 20);

    /* Hash the seed with SHA256 to produce the 32-byte secret */
    CeePewErr_t ceepew_err = crypto_sha256_compute(seed_buf, sizeof(seed_buf), secret_out);
    ceepew_secure_zero(seed_buf, sizeof(seed_buf));
    
    return ceepew_err;
}

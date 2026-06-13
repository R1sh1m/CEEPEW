/* components/crypto/crypto_hmac_efuse.c */

#include "crypto_hmac_efuse.h"
#include "crypto_hmac.h"
#include "crypto_sha256.h"
#include "ceepew_assert.h"
#include "ceepew_security_utils.h"
#include <string.h>

#include "esp_random.h"
#include "esp_log.h"
#include "esp_efuse.h"
#include "esp_efuse_chip.h"

static const char *TAG = "crypto_hmac_efuse";

/* ESP32 eFuse block 3 (user data): 256 bits total.
 * We use a 32-byte region starting at bit offset 64 (byte offset 8)
 * which leaves room for the 6-byte MAC (bytes 0-5) and the 2-byte
 * padding/version field (bytes 6-7). */
#define EFUSE_USER_BLK3        EFUSE_BLK3
#define EFUSE_IDENTITY_OFFSET  64U   /* bits — past the 8-byte MAC header */
#define EFUSE_IDENTITY_SIZE    256U  /* bits = 32 bytes */

bool crypto_hmac_efuse_is_provisioned(void) {
    uint8_t buf[32U];
    esp_err_t err = esp_efuse_read_block(EFUSE_USER_BLK3, buf,
                                          EFUSE_IDENTITY_OFFSET,
                                          EFUSE_IDENTITY_SIZE);
    if (err != ESP_OK) { return false; }

    /* Factory-fresh eFuse reads as all 0xFF.  If all bytes are 0xFF
     * or all zeros, the block is unprovisioned. */
    bool all_ff = true;
    bool all_zero = true;
    for (uint32_t i = 0U; i < sizeof(buf); i++) {
        if (buf[i] != 0xFFU) { all_ff = false; }
        if (buf[i] != 0x00U) { all_zero = false; }
    }
    return (!all_ff && !all_zero);
}

CeePewErr_t crypto_hmac_efuse_provision(void) {
    if (crypto_hmac_efuse_is_provisioned()) {
        ESP_LOGW(TAG, "eFuse identity already provisioned");
        return CEEPEW_OK;
    }

    uint8_t key[32U];
    esp_fill_random(key, sizeof(key));

    /* Ensure the key doesn't read as all-0xFF (factory default) or all-zero,
     * which could be confused with an unprovisioned block. */
    bool all_ff, all_zero;
    do {
        all_ff = true;
        all_zero = true;
        for (uint32_t i = 0U; i < sizeof(key); i++) {
            if (key[i] != 0xFFU) { all_ff = false; }
            if (key[i] != 0x00U) { all_zero = false; }
        }
        if (all_ff || all_zero) {
            esp_fill_random(key, sizeof(key));
        }
    } while (all_ff || all_zero);

    esp_err_t err = esp_efuse_write_block(EFUSE_USER_BLK3, key,
                                           EFUSE_IDENTITY_OFFSET,
                                           EFUSE_IDENTITY_SIZE);

    volatile uint8_t *vk = key;
    for (uint32_t i = 0U; i < sizeof(key); i++) { vk[i] = 0U; }
    __asm__ __volatile__("" ::: "memory");

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write eFuse identity: %s", esp_err_to_name(err));
        return CEEPEW_ERR_HW;
    }

    ESP_LOGI(TAG, "eFuse identity provisioned successfully");
    return CEEPEW_OK;
}

CeePewErr_t crypto_hmac_efuse_read_key(uint8_t key_out[32U]) {
    CEEPEW_ASSERT(key_out != NULL, CEEPEW_ERR_NULL_PTR);

    if (!crypto_hmac_efuse_is_provisioned()) {
        return CEEPEW_ERR_NOENT;
    }

    esp_err_t err = esp_efuse_read_block(EFUSE_USER_BLK3, key_out,
                                          EFUSE_IDENTITY_OFFSET,
                                          EFUSE_IDENTITY_SIZE);
    if (err != ESP_OK) {
        return CEEPEW_ERR_HW;
    }
    return CEEPEW_OK;
}

CeePewErr_t crypto_hmac_efuse_derive_salt(const uint8_t ds_mix[32U],
                                           const uint8_t code[32U],
                                           uint8_t hkdf_salt[32U]) {
    CEEPEW_ASSERT(ds_mix != NULL && code != NULL && hkdf_salt != NULL,
                  CEEPEW_ERR_NULL_PTR);

    if (!crypto_hmac_efuse_is_provisioned()) {
        ESP_LOGW(TAG, "eFuse identity not provisioned — falling back to "
                      "software-only salt derivation");
        uint8_t salt_input[64U];
        memcpy(salt_input, ds_mix, 32U);
        memcpy(salt_input + 32U, code, 32U);
        CeePewErr_t err = crypto_sha256_compute(salt_input, 64U, hkdf_salt);
        volatile uint8_t *vsi = salt_input;
        for (uint32_t i = 0U; i < sizeof(salt_input); i++) { vsi[i] = 0U; }
        __asm__ __volatile__("" ::: "memory");
        return err;
    }

    uint8_t id_key[32U];
    CeePewErr_t err = crypto_hmac_efuse_read_key(id_key);
    if (err != CEEPEW_OK) {
        return err;
    }

    uint8_t hmac_tag[32U];
    err = crypto_hmac_sha256(id_key, CEEPEW_EFUSE_IDENTITY_KEY_LEN,
                             ds_mix, 32U, hmac_tag);
    ceepew_secure_zero(id_key, sizeof(id_key));
    if (err != CEEPEW_OK) {
        return err;
    }

    uint8_t base_salt[32U];
    uint8_t salt_input[64U];
    memcpy(salt_input, ds_mix, 32U);
    memcpy(salt_input + 32U, code, 32U);
    err = crypto_sha256_compute(salt_input, 64U, base_salt);
    volatile uint8_t *vsi = salt_input;
    for (uint32_t i = 0U; i < sizeof(salt_input); i++) { vsi[i] = 0U; }
    __asm__ __volatile__("" ::: "memory");
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(hmac_tag, sizeof(hmac_tag));
        return err;
    }

    uint8_t combined[64U];
    memcpy(combined, hmac_tag, 32U);
    memcpy(combined + 32U, base_salt, 32U);
    ceepew_secure_zero(hmac_tag, sizeof(hmac_tag));
    ceepew_secure_zero(base_salt, sizeof(base_salt));

    err = crypto_sha256_compute(combined, 64U, hkdf_salt);
    volatile uint8_t *vc = combined;
    for (uint32_t i = 0U; i < sizeof(combined); i++) { vc[i] = 0U; }
    __asm__ __volatile__("" ::: "memory");

    return err;
}

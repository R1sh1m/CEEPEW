/* components/ceepew_hal/hal_radio.c */

#include "hal_radio.h"
#include "hal_pins.h"
#include "../../main/ceepew_config.h"
#include "../../main/ceepew_assert.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef CEEPEW_ESPNOW_CHANNEL
#define CEEPEW_ESPNOW_CHANNEL 1U
#endif

static const char *TAG = "hal_radio";

static bool s_initialised = false;
static bool s_peer_registered = false;
static uint8_t s_peer_mac[6] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
static uint8_t s_registered_peer_mac[6] = {0U, 0U, 0U, 0U, 0U, 0U};

static bool radio_mac_is_zero(const uint8_t mac[6])
{
    bool all_zero = true;
    for (uint8_t i = 0U; i < 6U; i++)
    {
        if (mac[i] != 0U)
        {
            all_zero = false;
            break;
        }
    }
    return all_zero;
}

static bool radio_mac_equal(const uint8_t lhs[6], const uint8_t rhs[6])
{
    for (uint8_t i = 0U; i < 6U; i++)
    {
        if (lhs[i] != rhs[i])
        {
            return false;
        }
    }
    return true;
}

static void radio_send_cb(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status == ESP_NOW_SEND_SUCCESS)
    {
        ESP_LOGD(TAG, "ESP-NOW send complete");
    }
    else
    {
        ESP_LOGW(TAG, "ESP-NOW send failed");
    }
}

static void radio_cleanup_partial(void)
{
    (void)esp_now_deinit();
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
    s_initialised = false;
    s_peer_registered = false;
}

static CeePewErr_t radio_register_peer(void)
{
    CEEPEW_ASSERT(s_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(CEEPEW_ESPNOW_CHANNEL >= 1U && CEEPEW_ESPNOW_CHANNEL <= 13U, CEEPEW_ERR_PARAM);

    if (s_peer_registered && radio_mac_equal(s_registered_peer_mac, s_peer_mac))
    {
        return CEEPEW_OK;
    }

    if (s_peer_registered)
    {
        esp_err_t rc = esp_now_del_peer(s_registered_peer_mac);
        if ((rc != ESP_OK) && (rc != ESP_ERR_ESPNOW_NOT_FOUND))
        {
            return CEEPEW_ERR_HW;
        }
        s_peer_registered = false;
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_peer_mac, sizeof(s_peer_mac));
    peer.ifidx = WIFI_IF_STA;
    peer.channel = (uint8_t)CEEPEW_ESPNOW_CHANNEL;
    peer.encrypt = false;

    esp_err_t rc = esp_now_add_peer(&peer);
    if (rc != ESP_OK)
    {
        return CEEPEW_ERR_HW;
    }

    memcpy(s_registered_peer_mac, s_peer_mac, sizeof(s_peer_mac));
    s_peer_registered = true;
    return CEEPEW_OK;
}

static CeePewErr_t radio_prepare_wifi(void)
{
    CEEPEW_ASSERT(CEEPEW_ESPNOW_CHANNEL >= 1U && CEEPEW_ESPNOW_CHANNEL <= 13U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(!radio_mac_is_zero(s_peer_mac), CEEPEW_ERR_PARAM);

    esp_err_t rc = nvs_flash_init();
    if ((rc == ESP_ERR_NVS_NO_FREE_PAGES) || (rc == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        rc = nvs_flash_erase();
        if (rc != ESP_OK)
        {
            return CEEPEW_ERR_HW;
        }
        rc = nvs_flash_init();
    }
    if (rc != ESP_OK)
    {
        return CEEPEW_ERR_HW;
    }

    rc = esp_netif_init();
    if ((rc != ESP_OK) && (rc != ESP_ERR_INVALID_STATE))
    {
        return CEEPEW_ERR_HW;
    }

    rc = esp_event_loop_create_default();
    if ((rc != ESP_OK) && (rc != ESP_ERR_INVALID_STATE))
    {
        return CEEPEW_ERR_HW;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    rc = esp_wifi_init(&cfg);
    if (rc != ESP_OK)
    {
        return CEEPEW_ERR_HW;
    }

    rc = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (rc != ESP_OK)
    {
        radio_cleanup_partial();
        return CEEPEW_ERR_HW;
    }

    rc = esp_wifi_set_mode(WIFI_MODE_STA);
    if (rc != ESP_OK)
    {
        radio_cleanup_partial();
        return CEEPEW_ERR_HW;
    }

    rc = esp_wifi_set_ps(WIFI_PS_NONE);
    if (rc != ESP_OK)
    {
        radio_cleanup_partial();
        return CEEPEW_ERR_HW;
    }

    rc = esp_wifi_start();
    if (rc != ESP_OK)
    {
        radio_cleanup_partial();
        return CEEPEW_ERR_HW;
    }

    rc = esp_wifi_set_channel((uint8_t)CEEPEW_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (rc != ESP_OK)
    {
        radio_cleanup_partial();
        return CEEPEW_ERR_HW;
    }

    return CEEPEW_OK;
}

CeePewErr_t hal_radio_init(void)
{
    CEEPEW_ASSERT(!s_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(CEEPEW_ESPNOW_CHANNEL >= 1U && CEEPEW_ESPNOW_CHANNEL <= 13U, CEEPEW_ERR_PARAM);

    CeePewErr_t err = radio_prepare_wifi();
    if (err != CEEPEW_OK)
    {
        return err;
    }

    esp_err_t rc = esp_now_init();
    if (rc != ESP_OK)
    {
        radio_cleanup_partial();
        return CEEPEW_ERR_HW;
    }

    rc = esp_now_register_send_cb(radio_send_cb);
    if (rc != ESP_OK)
    {
        radio_cleanup_partial();
        return CEEPEW_ERR_HW;
    }

    s_initialised = true;
    err = radio_register_peer();
    if (err != CEEPEW_OK)
    {
        radio_cleanup_partial();
        return err;
    }

    ESP_LOGI(TAG, "ESP-NOW initialised on channel %u", (unsigned)CEEPEW_ESPNOW_CHANNEL);
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_set_peer(const uint8_t peer_mac[6])
{
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(!radio_mac_is_zero(peer_mac), CEEPEW_ERR_PARAM);

    memcpy(s_peer_mac, peer_mac, sizeof(s_peer_mac));
    if (!s_initialised)
    {
        return CEEPEW_OK;
    }

    return radio_register_peer();
}

CeePewErr_t hal_radio_send(const uint8_t *buf, uint16_t len)
{
    CEEPEW_ASSERT(s_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(len <= ESP_NOW_MAX_DATA_LEN, CEEPEW_ERR_BOUNDS);

    esp_err_t rc = esp_now_send(s_peer_mac, buf, len);
    if (rc == ESP_OK)
    {
        return CEEPEW_OK;
    }
    if (rc == ESP_ERR_ESPNOW_ARG)
    {
        return CEEPEW_ERR_PARAM;
    }
    return CEEPEW_ERR_HW;
}

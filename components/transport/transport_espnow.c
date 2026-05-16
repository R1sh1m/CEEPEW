/* components/transport/transport_espnow.c */

#include "hal_input.h"
#include "../../main/ceepew_assert.h"
#include "hal_pins.h"
#include <stdint.h>

CeePewErr_t transport_espnow_init(void)
{
    CEEPEW_ASSERT(CEEPEW_ESPNOW_CHANNEL >= 1 && CEEPEW_ESPNOW_CHANNEL <= 13, CEEPEW_ERR_PARAM);
    return CEEPEW_OK;
}

CeePewErr_t transport_espnow_send(const uint8_t *peer_mac, const uint8_t *data, uint16_t len)
{
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(data != NULL || len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len <= 1024U, CEEPEW_ERR_BOUNDS);

    (void)peer_mac; (void)data; (void)len;
    return CEEPEW_ERR_UNSUPPORTED;
}

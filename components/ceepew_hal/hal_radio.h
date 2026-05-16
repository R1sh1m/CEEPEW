/* components/ceepew_hal/hal_radio.h */
#ifndef CEEPEW_HAL_RADIO_H
#define CEEPEW_HAL_RADIO_H

#include <stdint.h>
#include "../../main/ceepew_assert.h"

CeePewErr_t hal_radio_init(void);
CeePewErr_t hal_radio_set_peer(const uint8_t peer_mac[6]);
CeePewErr_t hal_radio_send(const uint8_t *buf, uint16_t len);

#endif /* CEEPEW_HAL_RADIO_H */

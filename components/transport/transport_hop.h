/* components/transport/transport_hop.h */

#ifndef TRANSPORT_HOP_H
#define TRANSPORT_HOP_H

#include <stdint.h>
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "../crypto/crypto_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Derive the current ESP-NOW channel for the active session using a
 * PRG-derived permutation of BASE_CHANNELS. Deterministic given same
 * crypto context, session key, and nonce state. */
CeePewErr_t transport_get_current_channel(const CryptoCtx_t *ctx,
                                         uint64_t nonce_counter,
                                         uint8_t *channel_out);

/* Invalidate the cached hop key. Must be called when session ends
 * (session_wipe / session_end) to prevent key material reuse. */
void transport_hop_invalidate_key(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_HOP_H */

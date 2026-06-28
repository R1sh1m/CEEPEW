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

/* Get current hopped channel for session. Deterministic from crypto ctx + nonce. */
CeePewErr_t transport_get_current_channel(const CryptoCtx_t *ctx,
                                         uint64_t nonce_counter,
                                         uint8_t *channel_out);

/* Invalidate cached hop key. Call on session end/wipe to prevent key reuse. */
void transport_hop_invalidate_key(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_HOP_H */

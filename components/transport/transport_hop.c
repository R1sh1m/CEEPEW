/* components/transport/transport_hop.c
 *
 * Implements a PRG-derived Fisher-Yates permutation over BASE_CHANNELS.
 * Seed is formed from ctx->ascon_key[0:3] (big-endian) XORed with hop_idx.
 * The PRG is a simple LCG (32-bit) used to generate per-swap randomness.
 */

#include "transport_hop.h"
#include <string.h>

/* Define BASE_CHANNELS per spec. There are CEEPEW_HOP_CHANNELS entries. 
 * Using contiguous channels 1..N for the default region. */
static const uint8_t BASE_CHANNELS[CEEPEW_HOP_CHANNELS] = {
    1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U
};

CeePewErr_t transport_get_current_channel(const CryptoCtx_t *ctx, uint8_t *channel_out){
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(channel_out != NULL, CEEPEW_ERR_NULL_PTR);

    /* Build 24-bit seed from session key bytes [0:3] (big-endian) */
    uint32_t seed = ((uint32_t)ctx->ascon_key[0] << 16) |
                    ((uint32_t)ctx->ascon_key[1] << 8)  |
                    ((uint32_t)ctx->ascon_key[2]);

    /* hop index derived from nonce counter shifted by CEEPEW_HOP_SHIFT */
    uint8_t hop_idx = (uint8_t)((ctx->nonce_counter >> CEEPEW_HOP_SHIFT) & 0xFFULL);

    seed ^= (uint32_t)hop_idx;

    /* Copy base channels into local mutable array */
    uint8_t perm[CEEPEW_HOP_CHANNELS];
    (void)memcpy(perm, BASE_CHANNELS, sizeof(perm));

    /* LCG state (32-bit). Constants chosen for reasonable distribution. */
    uint32_t state = seed;

    /* Fisher-Yates shuffle: i from N-1 down to 1 (exact loop bounds) */
    for (uint8_t i = (uint8_t)(CEEPEW_HOP_CHANNELS - 1U); i > 0U; i--) {
        /* Advance LCG */
        state = (uint32_t)((uint64_t)state * 1664525UL + 1013904223UL);
        /* j in [0, i] */
        uint8_t j = (uint8_t)(state % (uint32_t)(i + 1U));
        uint8_t tmp = perm[i];
        perm[i] = perm[j];
        perm[j] = tmp;
    }

    /* Select index within permutation for this hop round. Use low bits of hop_idx */
    uint8_t sel = (uint8_t)((ctx->nonce_counter >> CEEPEW_HOP_SHIFT) % (uint64_t)CEEPEW_HOP_CHANNELS);
    *channel_out = perm[sel];
    return CEEPEW_OK;
}

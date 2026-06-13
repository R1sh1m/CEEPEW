/* components/transport/transport_hop.c
 *
 * Implements a PRG-derived Fisher-Yates permutation over BASE_CHANNELS.
 * PRG uses HMAC-SHA256-based key derivation (replacing MurmurHash3).
 * Hop key = HMAC-SHA256(ascon_key[0:16], session_id[0:8]), cached on first use.
 *
 * THREAD SAFETY: The hop key cache is protected by a portMUX spinlock.
 * hop_ensure_key() computes the HMAC outside the critical section, then
 * installs the result inside it.  transport_hop_invalidate_key() zeros
 * the key inside the critical section so that a concurrent
 * transport_get_current_channel() never reads partially-zeroed material.
 */

#include "transport_hop.h"
#include "crypto_hmac.h"
#include "ceepew_security_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

/* Define BASE_CHANNELS per spec. There are CEEPEW_HOP_CHANNELS entries. 
 * Using contiguous channels 1..N for the default region. */
static const uint8_t BASE_CHANNELS[CEEPEW_HOP_CHANNELS] = {
    1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U
};

static uint8_t s_hop_key[32U];
static uint8_t s_cached_ascon_key[CEEPEW_SESSION_KEY_BYTES];
static uint8_t s_cached_session_id[8U];
static uint8_t s_hop_key_valid = 0U;
static portMUX_TYPE s_hop_mux = portMUX_INITIALIZER_UNLOCKED;

/* Design note: Caching the derived hopping PRG key avoids re-calculating the HMAC-SHA256
 * key derivation on every packet channel lookup. To ensure correctness across different
 * crypto contexts (e.g. during test suites or multi-session transitions), we validate
 * the cached key against the active context's session key and session ID. If they differ,
 * the cache is automatically invalidated and regenerated.
 *
 * THREAD SAFETY: The cache check + install is done inside a spinlock critical section.
 * The expensive HMAC computation happens OUTSIDE the lock to minimise lock hold time. */
static CeePewErr_t hop_ensure_key(const CryptoCtx_t *ctx)
{
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(ctx->session_active, CEEPEW_ERR_PARAM);

    /* Fast path: check cache under spinlock */
    taskENTER_CRITICAL(&s_hop_mux);
    if (s_hop_key_valid != 0U &&
        ceepew_ct_equal(s_cached_ascon_key, ctx->ascon_key, sizeof(s_cached_ascon_key)) == 1U &&
        ceepew_ct_equal(s_cached_session_id, ctx->session_id, sizeof(s_cached_session_id)) == 1U) {
        taskEXIT_CRITICAL(&s_hop_mux);
        return CEEPEW_OK;
    }
    taskEXIT_CRITICAL(&s_hop_mux);

    /* Cache miss — compute HMAC outside critical section */
    uint8_t new_hop_key[32U];
    CeePewErr_t err = crypto_hmac_sha256(
        ctx->ascon_key, sizeof(ctx->ascon_key),
        ctx->session_id, sizeof(ctx->session_id),
        new_hop_key);
    if (err != CEEPEW_OK) { return err; }

    /* Install result under spinlock */
    taskENTER_CRITICAL(&s_hop_mux);
    (void)memcpy(s_hop_key, new_hop_key, sizeof(s_hop_key));
    (void)memcpy(s_cached_ascon_key, ctx->ascon_key, sizeof(s_cached_ascon_key));
    (void)memcpy(s_cached_session_id, ctx->session_id, sizeof(s_cached_session_id));
    s_hop_key_valid = 1U;
    taskEXIT_CRITICAL(&s_hop_mux);

    ceepew_secure_zero(new_hop_key, sizeof(new_hop_key));
    return CEEPEW_OK;
}

void transport_hop_invalidate_key(void)
{
    taskENTER_CRITICAL(&s_hop_mux);
    ceepew_secure_zero(s_hop_key, sizeof(s_hop_key));
    ceepew_secure_zero(s_cached_ascon_key, sizeof(s_cached_ascon_key));
    ceepew_secure_zero(s_cached_session_id, sizeof(s_cached_session_id));
    s_hop_key_valid = 0U;
    taskEXIT_CRITICAL(&s_hop_mux);
}

CeePewErr_t transport_get_current_channel(const CryptoCtx_t *ctx,
                                         uint64_t nonce_counter,
                                         uint8_t *channel_out){
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(channel_out != NULL, CEEPEW_ERR_NULL_PTR);

    CeePewErr_t err = hop_ensure_key(ctx);
    if (err != CEEPEW_OK) { return err; }

    /* Snapshot the hop key under spinlock so it cannot be zeroed
     * by a concurrent transport_hop_invalidate_key() mid-computation. */
    uint8_t local_key[32U];
    taskENTER_CRITICAL(&s_hop_mux);
    (void)memcpy(local_key, s_hop_key, sizeof(local_key));
    taskEXIT_CRITICAL(&s_hop_mux);

    uint8_t perm[CEEPEW_HOP_CHANNELS];
    (void)memcpy(perm, BASE_CHANNELS, sizeof(perm));

    uint8_t prf_out[32U];
    uint8_t hop_idx = (uint8_t)((nonce_counter >> CEEPEW_HOP_SHIFT) & 0xFFULL);

    /* Use local_key for all HMAC-SHA256 PRF calls */
    uint8_t msg[4U];
    msg[0] = (uint8_t)((hop_idx >> 24U) & 0xFFU);
    msg[1] = (uint8_t)((hop_idx >> 16U) & 0xFFU);
    msg[2] = (uint8_t)((hop_idx >> 8U)  & 0xFFU);
    msg[3] = (uint8_t)((hop_idx)        & 0xFFU);
    (void)crypto_hmac_sha256(local_key, sizeof(local_key), msg, sizeof(msg), prf_out);

    uint32_t state = ((uint32_t)prf_out[0] << 24) |
                     ((uint32_t)prf_out[1] << 16) |
                     ((uint32_t)prf_out[2] << 8)  |
                     ((uint32_t)prf_out[3]);

    for (uint8_t i = (uint8_t)(CEEPEW_HOP_CHANNELS - 1U); i > 0U; i--) {
        msg[0] = (uint8_t)((((uint32_t)hop_idx * 100U + (uint32_t)i) >> 24U) & 0xFFU);
        msg[1] = (uint8_t)((((uint32_t)hop_idx * 100U + (uint32_t)i) >> 16U) & 0xFFU);
        msg[2] = (uint8_t)((((uint32_t)hop_idx * 100U + (uint32_t)i) >> 8U)  & 0xFFU);
        msg[3] = (uint8_t)(((uint32_t)hop_idx * 100U + (uint32_t)i)        & 0xFFU);
        (void)crypto_hmac_sha256(local_key, sizeof(local_key), msg, sizeof(msg), prf_out);

        state = ((uint32_t)prf_out[0] << 24) |
                ((uint32_t)prf_out[1] << 16) |
                ((uint32_t)prf_out[2] << 8)  |
                ((uint32_t)prf_out[3]);

        uint8_t j = (uint8_t)(state % (uint32_t)(i + 1U));
        uint8_t tmp = perm[i];
        perm[i] = perm[j];
        perm[j] = tmp;
    }

    ceepew_secure_zero(local_key, sizeof(local_key));

    uint8_t sel = (uint8_t)((nonce_counter >> (CEEPEW_HOP_SHIFT + 8U)) % (uint64_t)CEEPEW_HOP_CHANNELS);
    *channel_out = perm[sel];
    return CEEPEW_OK;
}

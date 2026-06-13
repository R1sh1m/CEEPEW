/* components/crypto/crypto_rng.c */

#include "crypto_rng.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_security_utils.h"
#include "esp_system.h"
#include "esp_random.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CEEPEW_RNG_CHUNK_BYTES 32U
#define CEEPEW_RNG_FAILURE_THRESHOLD 3U

typedef struct {
    uint8_t prev_sample[CEEPEW_RNG_CHUNK_BYTES];
    uint8_t has_prev;
    uint32_t failure_count;
    crypto_rng_failure_cb_t failure_cb;
} RngHealthState_t;

static RngHealthState_t s_rng_health;

static CeePewErr_t rng_fill_esp(uint8_t *buf, uint32_t len) {
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_BOUNDS);
    uint32_t offset = 0U;
    /* Fill using esp_random() (returns 32-bit random). Copy in 4-byte chunks. */
    while (offset < len) {
        uint32_t rnd = esp_random();
        uint32_t remain = len - offset;
        uint32_t to_copy = (remain < 4U) ? remain : 4U;
        /* copy little-endian bytes of rnd into buffer */
        for (uint32_t i = 0U; i < to_copy; i++) {
            buf[offset + i] = (uint8_t)((rnd >> (8U * i)) & 0xFFU);
        }
        offset += to_copy;
    }

    uint8_t non_zero_acc = 0U;
    /* loop bound: len validated <= CEEPEW_REGION_POOL_BYTES */
    for (uint32_t i = 0U; i < len; i++) { non_zero_acc |= buf[i]; }
    CEEPEW_ASSERT(non_zero_acc != 0U, CEEPEW_ERR_CRYPTO);
    return CEEPEW_OK;
}

void crypto_rng_set_failure_callback(crypto_rng_failure_cb_t cb)
{
    s_rng_health.failure_cb = cb;
}

uint32_t crypto_rng_get_failure_count(void)
{
    return s_rng_health.failure_count;
}

void crypto_rng_reset_health_state(void)
{
    ceepew_secure_zero(&s_rng_health.prev_sample[0], CEEPEW_RNG_CHUNK_BYTES);
    s_rng_health.has_prev = 0U;
    s_rng_health.failure_count = 0U;
    s_rng_health.failure_cb = NULL;
}

CeePewErr_t crypto_rng_continuous_test(const uint8_t *sample, uint32_t len)
{
    if (len < CEEPEW_RNG_CHUNK_BYTES) { return CEEPEW_OK; }

    if (s_rng_health.has_prev != 0U) {
        uint8_t diff_acc = 0U;
        for (uint32_t i = 0U; i < CEEPEW_RNG_CHUNK_BYTES; i++) {
            diff_acc |= (uint8_t)(sample[i] ^ s_rng_health.prev_sample[i]);
        }
        if (diff_acc == 0U) {
            s_rng_health.failure_count++;
            if (s_rng_health.failure_count >= CEEPEW_RNG_FAILURE_THRESHOLD) {
                if (s_rng_health.failure_cb != NULL) { s_rng_health.failure_cb(); }
            }
            return CEEPEW_ERR_CRYPTO;
        }
        s_rng_health.failure_count = 0U;
    }

    memcpy(s_rng_health.prev_sample, sample, CEEPEW_RNG_CHUNK_BYTES);
    s_rng_health.has_prev = 1U;
    return CEEPEW_OK;
}

CeePewErr_t crypto_rng_fill(uint8_t *buf, uint32_t len) {
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_BOUNDS);
    CeePewErr_t err = rng_fill_esp(buf, len);
    if (err != CEEPEW_OK) { return err; }
    if (len >= CEEPEW_RNG_CHUNK_BYTES) {
        err = crypto_rng_continuous_test(buf, len);
        if (err != CEEPEW_OK) { return err; }
    }
    return CEEPEW_OK;
}

CeePewErr_t crypto_rng_health_check(void)
{
    /* Ensure compile-time chunk size sanity at runtime */
    CEEPEW_ASSERT(CEEPEW_RNG_CHUNK_BYTES > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(CEEPEW_RNG_CHUNK_BYTES <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_PARAM);

    uint8_t a[CEEPEW_RNG_CHUNK_BYTES];
    uint8_t b[CEEPEW_RNG_CHUNK_BYTES];
    uint8_t c[CEEPEW_RNG_CHUNK_BYTES];

    CeePewErr_t err = rng_fill_esp(a, CEEPEW_RNG_CHUNK_BYTES);
    if (err != CEEPEW_OK) { return err; }

    err = rng_fill_esp(b, CEEPEW_RNG_CHUNK_BYTES);
    if (err != CEEPEW_OK) { return err; }

    err = rng_fill_esp(c, CEEPEW_RNG_CHUNK_BYTES);
    if (err != CEEPEW_OK) { return err; }

    /* Basic pairwise uniqueness checks — identical consecutive outputs indicate RNG failure */
    uint8_t acc_ab = 0U, acc_bc = 0U, acc_ac = 0U, acc_all = 0U;
    uint32_t sum_a = 0U;
    for (uint32_t i = 0U; i < CEEPEW_RNG_CHUNK_BYTES; i++) {
        acc_ab |= (uint8_t)(a[i] ^ b[i]);
        acc_bc |= (uint8_t)(b[i] ^ c[i]);
        acc_ac |= (uint8_t)(a[i] ^ c[i]);
        acc_all |= (uint8_t)(a[i] ^ b[i] ^ c[i]);
        sum_a += (uint32_t)a[i];
    }

    /* If any pairwise XOR is zero, two samples were identical — fail fast */
    CEEPEW_ASSERT(acc_ab != 0U && acc_bc != 0U && acc_ac != 0U && acc_all != 0U,
                  CEEPEW_ERR_CRYPTO);

    /* Heuristic: check that the first sample has non-trivial byte variety (not all zeros) */
    CEEPEW_ASSERT(sum_a != 0U, CEEPEW_ERR_CRYPTO);

    /* Secure zero temporaries */
    ceepew_secure_zero(a, CEEPEW_RNG_CHUNK_BYTES);
    ceepew_secure_zero(b, CEEPEW_RNG_CHUNK_BYTES);
    ceepew_secure_zero(c, CEEPEW_RNG_CHUNK_BYTES);

    return CEEPEW_OK;
}

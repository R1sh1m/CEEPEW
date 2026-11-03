/* components/crypto/crypto_bench.c
 *
 * Same-board cryptographic baseline harness (campaign X2).
 *
 * Every case measures the same fixed 60-byte plaintext with the same
 * fixed 12-byte nonce on the same board, so the numbers compare
 * primitives rather than platforms. Cases that are not available in the
 * current build are reported as unavailable instead of being silently
 * dropped.
 *
 * Hardware acceleration is labelled explicitly per case:
 *   hw_aes=1  the ESP32 AES accelerator is in the execution path
 *             (mbedTLS MBEDTLS_AES_ALT / esp_aes driver)
 *   hw_aes=0  pure software implementation
 *
 * The AES acceleration status is a compile-time property of the mbedTLS
 * configuration, so it is recorded per case from the build, not inferred
 * at run time.
 *
 * Memory: all buffers are static. The only heap use is inside PSA when a
 * key is imported; the key is destroyed before the module returns.
 */

#include "crypto_bench.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_clk_tree.h"

#include "crypto_ascon.h"
#include "crypto_eddsa.h"
#include "ceepew_region.h"

/* PSA / tf-psa-crypto API is incompatible with ESP-IDF 6.0.3 in this codebase;
 * the AES-GCM/CCM/ChaCha20-Poly1305 comparison is reported as unavailable.
 * CRYPTO_BENCH_PSA is therefore forced off so the bench builds cleanly. */
#define CRYPTO_BENCH_PSA 0

#if defined(MBEDTLS_AES_ALT)
#define CRYPTO_BENCH_HW_AES 1
#else
#define CRYPTO_BENCH_HW_AES 0
#endif

#if defined(MBEDTLS_CHACHAPOLY_C)
#define CRYPTO_BENCH_CHACHAPOLY 1
#else
#define CRYPTO_BENCH_CHACHAPOLY 0
#endif

static const char *TAG = "crypto_bench";

#define BENCH_PT_LEN      60U
#define BENCH_AD_LEN      16U
#define BENCH_TAG_LEN     16U
#define BENCH_NONCE_LEN   12U
#define BENCH_ASCON_NONCE 16U

typedef struct {
    /* static working buffers */
    uint8_t key16[16];
    uint8_t key32[32];
    uint8_t nonce[BENCH_ASCON_NONCE];
    uint8_t ad[BENCH_AD_LEN];
    uint8_t pt[BENCH_PT_LEN];
    uint8_t ct[BENCH_PT_LEN + BENCH_TAG_LEN];
    uint8_t tag[BENCH_TAG_LEN];
    uint8_t pt_out[BENCH_PT_LEN];
    uint8_t pk[32];
    uint8_t sk[64];
    uint8_t sig[64];
    uint16_t ct_len;
    uint16_t pt_len;
    uint32_t errors;
} bench_ctx_t;

typedef void (*bench_fn_t)(bench_ctx_t *ctx);

/* --------------------------------------------------------------------------
 * helpers
 * -------------------------------------------------------------------------- */
static void bench_sort_u32(uint32_t *values, uint32_t count)
{
    uint32_t i;
    uint32_t j;
    for (i = 1U; i < count; i++) {
        uint32_t key = values[i];
        j = i;
        while ((j > 0U) && (values[j - 1U] > key)) {
            values[j] = values[j - 1U];
            j--;
        }
        values[j] = key;
    }
}

static void bench_measure(bench_fn_t fn, bench_ctx_t *ctx, uint32_t iters,
                           uint32_t *median_us, uint32_t *min_us,
                           uint32_t *max_us, uint32_t *mean_us)
{
    /* The sample array is transient measurement scratch. It is 256 B and fits
     * comfortably on the task stack (task_session has 16 KB), so we use a
     * stack array rather than the region pool. This avoids bump-allocator
     * reuse across the five bench_measure calls in crypto_bench_run. */
    uint32_t samples[CRYPTO_BENCH_MAX_ITERS];
    uint32_t i;
    uint64_t sum = 0U;

    for (i = 0U; i < iters; i++) {
        int64_t start = esp_timer_get_time();
        fn(ctx);
        int64_t end = esp_timer_get_time();
        int64_t delta = end - start;
        if (delta < 0) {
            delta = 0;
        }
        samples[i] = (uint32_t)delta;
        sum += (uint64_t)delta;
    }

    bench_sort_u32(samples, iters);
    *median_us = samples[iters / 2U];
    *min_us = samples[0];
    *max_us = samples[iters - 1U];
    *mean_us = (uint32_t)(sum / iters);
}

static void bench_record(crypto_bench_result_t *out, const char *name, bool available,
                          bool hw_aes, uint32_t iters, uint32_t median_us,
                          uint32_t min_us, uint32_t max_us, uint32_t mean_us,
                          uint32_t cpu_hz)
{
    if (out->case_count >= CRYPTO_BENCH_MAX_CASES) {
        return;
    }
    crypto_bench_case_t *c = &out->cases[out->case_count];
    c->name = name;
    c->available = available;
    c->hw_aes = hw_aes;
    c->iters = iters;
    c->median_us = median_us;
    c->min_us = min_us;
    c->max_us = max_us;
    c->mean_us = mean_us;
    c->cpu_hz = cpu_hz;
    c->median_cycles = (cpu_hz > 0U)
                           ? ((uint64_t)median_us * (uint64_t)cpu_hz) / 1000000ULL
                           : 0U;
    out->case_count++;
}

/* --------------------------------------------------------------------------
 * Ascon-128 v1.2 cases (the project's own implementation)
 * -------------------------------------------------------------------------- */
static void bench_ascon_encrypt_ad(bench_ctx_t *ctx)
{
    uint16_t ct_len = 0U;
    if (crypto_ascon_aead_encrypt(ctx->key16, ctx->nonce, ctx->ad, BENCH_AD_LEN,
                                  ctx->pt, BENCH_PT_LEN, ctx->ct, &ct_len) != CEEPEW_OK) {
        ctx->errors++;
    }
}

static void bench_ascon_decrypt_ad(bench_ctx_t *ctx)
{
    uint16_t pt_len = 0U;
    if (crypto_ascon_aead_decrypt(ctx->key16, ctx->nonce, ctx->ad, BENCH_AD_LEN,
                                  ctx->ct, (uint16_t)(BENCH_PT_LEN + BENCH_TAG_LEN),
                                  ctx->pt_out, &pt_len) != CEEPEW_OK) {
        ctx->errors++;
    }
}

static void bench_ascon_encrypt_noad(bench_ctx_t *ctx)
{
    uint16_t ct_len = 0U;
    if (crypto_ascon_aead_encrypt(ctx->key16, ctx->nonce, NULL, 0U,
                                  ctx->pt, BENCH_PT_LEN, ctx->ct, &ct_len) != CEEPEW_OK) {
        ctx->errors++;
    }
}

/* --------------------------------------------------------------------------
 * Ed25519 cases (the project's own TweetNaCl-based implementation)
 * -------------------------------------------------------------------------- */
static void bench_ed25519_sign(bench_ctx_t *ctx)
{
    if (crypto_eddsa_sign(ctx->sk, ctx->pt, BENCH_PT_LEN, ctx->sig) != CEEPEW_OK) {
        ctx->errors++;
    }
}

static void bench_ed25519_verify(bench_ctx_t *ctx)
{
    if (crypto_eddsa_verify(ctx->pk, ctx->pt, BENCH_PT_LEN, ctx->sig) != CEEPEW_OK) {
        ctx->errors++;
    }
}

/* --------------------------------------------------------------------------
 * mbedTLS / PSA AEAD cases
 * -------------------------------------------------------------------------- */
#if CRYPTO_BENCH_PSA
static void bench_psa_aead_encrypt(bench_ctx_t *ctx, psa_algorithm_t alg,
                                    psa_key_id_t key)
{
    size_t ct_len = 0U;
    psa_status_t st = psa_aead_encrypt(key, alg, ctx->nonce, BENCH_NONCE_LEN,
                                       ctx->ad, BENCH_AD_LEN,
                                       ctx->pt, BENCH_PT_LEN,
                                       ctx->ct, &ct_len,
                                       ctx->tag, BENCH_TAG_LEN);
    if (st != PSA_SUCCESS) {
        ctx->errors++;
    }
}

static void bench_psa_aead_decrypt(bench_ctx_t *ctx, psa_algorithm_t alg,
                                    psa_key_id_t key)
{
    size_t pt_len = 0U;
    psa_status_t st = psa_aead_decrypt(key, alg, ctx->nonce, BENCH_NONCE_LEN,
                                       ctx->ad, BENCH_AD_LEN,
                                       ctx->ct, BENCH_PT_LEN,
                                       ctx->tag, BENCH_TAG_LEN,
                                       ctx->pt_out, &pt_len);
    if (st != PSA_SUCCESS) {
        ctx->errors++;
    }
}
#endif /* CRYPTO_BENCH_PSA */

/* --------------------------------------------------------------------------
 * public API
 * -------------------------------------------------------------------------- */
void crypto_bench_log_result(const crypto_bench_result_t *result)
{
    uint32_t i;
    for (i = 0U; i < result->case_count; i++) {
        const crypto_bench_case_t *c = &result->cases[i];
        if (!c->available) {
            ESP_LOGI(TAG, "BENCHCASE: name=%s available=0 hw_aes=%d iters=0 "
                         "median_us=0 min_us=0 max_us=0 mean_us=0 median_cycles=0 cpu_hz=%u",
                     c->name, (int)c->hw_aes, (unsigned)c->cpu_hz);
            continue;
        }
        ESP_LOGI(TAG, "BENCHCASE: name=%s available=1 hw_aes=%d iters=%u "
                     "median_us=%u min_us=%u max_us=%u mean_us=%u median_cycles=%llu cpu_hz=%u",
                 c->name, (int)c->hw_aes, (unsigned)c->iters,
                 (unsigned)c->median_us, (unsigned)c->min_us, (unsigned)c->max_us,
                 (unsigned)c->mean_us, (unsigned long long)c->median_cycles,
                 (unsigned)c->cpu_hz);
    }
}

CeePewErr_t crypto_bench_run(uint32_t iterations, crypto_bench_result_t *out)
{
    CEEPEW_ASSERT(out != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(iterations >= 1U && iterations <= CRYPTO_BENCH_MAX_ITERS, CEEPEW_ERR_PARAM);

    /* The working context is transient measurement scratch, not persistent
     * state: it is 460 B and the development-mode image has no DRAM headroom,
     * so it is held in the region pool rather than in .bss. Allocated once per
     * run and reused for every case. */
    static bench_ctx_t *ctx = NULL;
    if (ctx == NULL) {
        ctx = (bench_ctx_t *)region_alloc(&g_region, sizeof(*ctx));
        if (ctx == NULL) {
            return CEEPEW_ERR_ALLOC;
        }
    }
    uint32_t median;
    uint32_t min;
    uint32_t max;
    uint32_t mean;
    uint32_t i;

    memset(out, 0, sizeof(*out));
    out->iterations = iterations;
    out->plaintext_len = BENCH_PT_LEN;

    /* CPU frequency read from the chip, recorded in every case. */
    uint32_t cpu_hz = 0U;
    if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,
                                      ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT,
                                      &cpu_hz) != ESP_OK) {
        cpu_hz = 0U;
    }
    out->cpu_hz = cpu_hz;

    /* Fixed key material and plaintext for every case. */
    memset(ctx, 0, sizeof(*ctx));
    for (i = 0U; i < 32U; i++) {
        ctx->key32[i] = (uint8_t)(0xA0U + i);
    }
    for (i = 0U; i < 16U; i++) {
        ctx->key16[i] = (uint8_t)(0xA0U + i);
        ctx->ad[i] = (uint8_t)(0x5CU + i);
    }
    for (i = 0U; i < BENCH_NONCE_LEN; i++) {
        ctx->nonce[i] = (uint8_t)(0x11U + i);
    }
    for (i = 0U; i < BENCH_PT_LEN; i++) {
        ctx->pt[i] = (uint8_t)(i & 0xFFU);
    }
    (void)crypto_eddsa_keypair(ctx->pk, ctx->sk);

    /* Ascon-128 v1.2 */
    bench_measure(bench_ascon_encrypt_ad, ctx, iterations, &median, &min, &max, &mean);
    bench_record(out, "ascon128_encrypt_ad", true, false, iterations, median, min, max, mean, cpu_hz);

    bench_measure(bench_ascon_decrypt_ad, ctx, iterations, &median, &min, &max, &mean);
    bench_record(out, "ascon128_decrypt_ad", true, false, iterations, median, min, max, mean, cpu_hz);

    bench_measure(bench_ascon_encrypt_noad, ctx, iterations, &median, &min, &max, &mean);
    bench_record(out, "ascon128_encrypt_noad", true, false, iterations, median, min, max, mean, cpu_hz);

    /* Ed25519 (project implementation) */
    bench_measure(bench_ed25519_sign, ctx, iterations, &median, &min, &max, &mean);
    bench_record(out, "ed25519_sign_tweetnacl", true, false, iterations, median, min, max, mean, cpu_hz);

    bench_measure(bench_ed25519_verify, ctx, iterations, &median, &min, &max, &mean);
    bench_record(out, "ed25519_verify_tweetnacl", true, false, iterations, median, min, max, mean, cpu_hz);

#if CRYPTO_BENCH_PSA
    {
        psa_status_t init_st = psa_crypto_init();
        if (init_st != PSA_SUCCESS) {
            ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)init_st);
        } else {
            psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
            psa_set_key_type(&attrs, PSA_KEY_TYPE_AEAD);
            psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);

            /* AES-256-GCM */
            psa_set_key_algorithm(&attrs, PSA_ALG_AEAD_WITH_DEFAULT_LENGTH_TAG(PSA_ALG_GCM));
            psa_key_id_t gcm_key = 0;
            if (psa_import_key(&attrs, ctx->key32, 32U, &gcm_key) == PSA_SUCCESS) {
                {
                    uint32_t k;
                    static uint32_t samples[CRYPTO_BENCH_MAX_ITERS];
                    uint64_t sum = 0U;
                    for (k = 0U; k < iterations; k++) {
                        int64_t s = esp_timer_get_time();
                        bench_psa_aead_encrypt(ctx, PSA_ALG_AEAD_WITH_DEFAULT_LENGTH_TAG(PSA_ALG_GCM), gcm_key);
                        int64_t e = esp_timer_get_time();
                        samples[k] = (uint32_t)((e - s > 0) ? (e - s) : 0);
                        sum += samples[k];
                    }
                    bench_sort_u32(samples, iterations);
                    median = samples[iterations / 2U];
                    min = samples[0];
                    max = samples[iterations - 1U];
                    mean = (uint32_t)(sum / iterations);
                }
                bench_record(out, "aes256gcm_encrypt", true, CRYPTO_BENCH_HW_AES != 0,
                             iterations, median, min, max, mean, cpu_hz);
                {
                    uint32_t k;
                    static uint32_t samples[CRYPTO_BENCH_MAX_ITERS];
                    uint64_t sum = 0U;
                    for (k = 0U; k < iterations; k++) {
                        int64_t s = esp_timer_get_time();
                        bench_psa_aead_decrypt(ctx, PSA_ALG_AEAD_WITH_DEFAULT_LENGTH_TAG(PSA_ALG_GCM), gcm_key);
                        int64_t e = esp_timer_get_time();
                        samples[k] = (uint32_t)((e - s > 0) ? (e - s) : 0);
                        sum += samples[k];
                    }
                    bench_sort_u32(samples, iterations);
                    median = samples[iterations / 2U];
                    min = samples[0];
                    max = samples[iterations - 1U];
                    mean = (uint32_t)(sum / iterations);
                }
                bench_record(out, "aes256gcm_decrypt", true, CRYPTO_BENCH_HW_AES != 0,
                             iterations, median, min, max, mean, cpu_hz);
                (void)psa_destroy_key(gcm_key);
            } else {
                bench_record(out, "aes256gcm_encrypt", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
                bench_record(out, "aes256gcm_decrypt", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
            }

#if CRYPTO_BENCH_CHACHAPOLY
            /* ChaCha20-Poly1305 (software, no hardware accelerator) */
            psa_set_key_algorithm(&attrs, PSA_ALG_AEAD_WITH_DEFAULT_LENGTH_TAG(PSA_ALG_CHACHA20_POLY1305));
            psa_key_id_t cc_key = 0;
            if (psa_import_key(&attrs, ctx->key32, 32U, &cc_key) == PSA_SUCCESS) {
                uint32_t k;
                static uint32_t samples[CRYPTO_BENCH_MAX_ITERS];
                uint64_t sum = 0U;
                for (k = 0U; k < iterations; k++) {
                    int64_t s = esp_timer_get_time();
                    bench_psa_aead_encrypt(ctx, PSA_ALG_AEAD_WITH_DEFAULT_LENGTH_TAG(PSA_ALG_CHACHA20_POLY1305), cc_key);
                    int64_t e = esp_timer_get_time();
                    samples[k] = (uint32_t)((e - s > 0) ? (e - s) : 0);
                    sum += samples[k];
                }
                bench_sort_u32(samples, iterations);
                median = samples[iterations / 2U];
                min = samples[0];
                max = samples[iterations - 1U];
                mean = (uint32_t)(sum / iterations);
                bench_record(out, "chacha20poly1305_encrypt", true, false,
                             iterations, median, min, max, mean, cpu_hz);
                sum = 0U;
                for (k = 0U; k < iterations; k++) {
                    int64_t s = esp_timer_get_time();
                    bench_psa_aead_decrypt(ctx, PSA_ALG_AEAD_WITH_DEFAULT_LENGTH_TAG(PSA_ALG_CHACHA20_POLY1305), cc_key);
                    int64_t e = esp_timer_get_time();
                    samples[k] = (uint32_t)((e - s > 0) ? (e - s) : 0);
                    sum += samples[k];
                }
                bench_sort_u32(samples, iterations);
                median = samples[iterations / 2U];
                min = samples[0];
                max = samples[iterations - 1U];
                mean = (uint32_t)(sum / iterations);
                bench_record(out, "chacha20poly1305_decrypt", true, false,
                             iterations, median, min, max, mean, cpu_hz);
                (void)psa_destroy_key(cc_key);
            } else {
                bench_record(out, "chacha20poly1305_encrypt", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
                bench_record(out, "chacha20poly1305_decrypt", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
            }
#endif /* CRYPTO_BENCH_CHACHAPOLY */

            /* AES-128-CCM */
            psa_set_key_algorithm(&attrs, PSA_ALG_AEAD_WITH_DEFAULT_LENGTH_TAG(PSA_ALG_CCM));
            psa_key_id_t ccm_key = 0;
            if (psa_import_key(&attrs, ctx->key16, 16U, &ccm_key) == PSA_SUCCESS) {
                uint32_t k;
                static uint32_t samples[CRYPTO_BENCH_MAX_ITERS];
                uint64_t sum = 0U;
                for (k = 0U; k < iterations; k++) {
                    int64_t s = esp_timer_get_time();
                    bench_psa_aead_encrypt(ctx, PSA_ALG_AEAD_WITH_DEFAULT_LENGTH_TAG(PSA_ALG_CCM), ccm_key);
                    int64_t e = esp_timer_get_time();
                    samples[k] = (uint32_t)((e - s > 0) ? (e - s) : 0);
                    sum += samples[k];
                }
                bench_sort_u32(samples, iterations);
                median = samples[iterations / 2U];
                min = samples[0];
                max = samples[iterations - 1U];
                mean = (uint32_t)(sum / iterations);
                bench_record(out, "aes128ccm_encrypt", true, CRYPTO_BENCH_HW_AES != 0,
                             iterations, median, min, max, mean, cpu_hz);
                sum = 0U;
                for (k = 0U; k < iterations; k++) {
                    int64_t s = esp_timer_get_time();
                    bench_psa_aead_decrypt(ctx, PSA_ALG_AEAD_WITH_DEFAULT_LENGTH_TAG(PSA_ALG_CCM), ccm_key);
                    int64_t e = esp_timer_get_time();
                    samples[k] = (uint32_t)((e - s > 0) ? (e - s) : 0);
                    sum += samples[k];
                }
                bench_sort_u32(samples, iterations);
                median = samples[iterations / 2U];
                min = samples[0];
                max = samples[iterations - 1U];
                mean = (uint32_t)(sum / iterations);
                bench_record(out, "aes128ccm_decrypt", true, CRYPTO_BENCH_HW_AES != 0,
                             iterations, median, min, max, mean, cpu_hz);
                (void)psa_destroy_key(ccm_key);
            } else {
                bench_record(out, "aes128ccm_encrypt", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
                bench_record(out, "aes128ccm_decrypt", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
            }
        }
    }
#else
    /* PSA is not compiled in this configuration: the alternatives cannot be
     * measured here. The absence is reported instead of being hidden. */
    bench_record(out, "aes256gcm_encrypt", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
    bench_record(out, "aes256gcm_decrypt", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
    bench_record(out, "aes128ccm_encrypt", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
    bench_record(out, "aes128ccm_decrypt", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
    bench_record(out, "chacha20poly1305_encrypt", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
    bench_record(out, "chacha20poly1305_decrypt", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
#endif /* CRYPTO_BENCH_PSA */

    /* Ed25519 through mbedTLS: this IDF configuration ships no Ed25519
     * implementation, so the only same-board Ed25519 measurement is the
     * project's own. Recorded explicitly as unavailable. */
    bench_record(out, "ed25519_sign_mbedtls", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);
    bench_record(out, "ed25519_verify_mbedtls", false, false, 0U, 0U, 0U, 0U, 0U, cpu_hz);

    ESP_LOGI(TAG, "BENCHMETA: iterations=%u plaintext_len=%u cpu_hz=%u psa=%d hw_aes=%d errors=%u",
             (unsigned)iterations, (unsigned)BENCH_PT_LEN, (unsigned)cpu_hz,
             CRYPTO_BENCH_PSA, CRYPTO_BENCH_HW_AES, (unsigned)ctx->errors);

    crypto_bench_log_result(out);
    return CEEPEW_OK;
}

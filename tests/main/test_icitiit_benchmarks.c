/* tests/main/test_icitiit_benchmarks.c
 *
 * STEP 2: on-device micro-benchmark for the ICIT-IIT paper.
 *
 * Measures per-stage wall time (esp_timer_get_time, us) and CPU cycles
 * (esp_cpu_get_cycle_count / Xtensa CCOUNT, ESP-IDF v6.0.2) over the
 * worst-case 1-fragment message chain at N=200 iterations:
 *
 *   58B compressed -> 60B frag -> 76B Ascon -> 92B Outer Box -> 158B signed
 *   -> 220B FEC+CRC -> 248B ESL -> 249B ARQ frame
 *
 * (60 = 2B frag hdr + 58; 76 = 60 + 16 Ascon tag; 92 = 76 + 16 box tag;
 *  158 = 2B len + 92 + 64 sig; 216 = Hamming(15,11) of 158B, +4 CRC = 220;
 *  248 = 24 ESL hdr + 220 + 4 ESL CRC; 249 = 1 ARQ seq + 248.)
 *
 * BENCHMARK-ONLY CAVEATS (do not copy into production):
 *  - Fixed test keys/nonces; box encrypt reuses one nonce counter value
 *    (session inactive during tests). Timing only, NOT secure.
 *  - Ed25519 keypair uses crypto_eddsa_seeded_keypair (deterministic setup);
 *    the timed sign/verify path is identical to production.
 *  - ARQ/ESL timings include their ESP_LOGX call overhead (real code path).
 *  - vTaskDelay(1) every 20 iterations feeds the task watchdog; it sits
 *    OUTSIDE the timed windows.
 *
 * Zero-heap: all storage is static (.bss); no malloc/free. Region allocator
 * is only touched indirectly by transport_esl_process_incoming (pass-through
 * stages, no allocs); HWM snapshots prove it.
 */

#include "test_icitiit_benchmarks.h"

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_region.h"
#include "ceepew_security_utils.h"
#include "session_fsm.h"
#include "compress_huffman.h"
#include "curve25519.h"
#include "crypto_ecdh.h"
#include "digital_sum.h"
#include "crypto_sha256.h"
#include "crypto_hkdf.h"
#include "crypto_ascon.h"
#include "crypto_box_wrap.h"
#include "crypto_ctx.h"
#include "crypto_eddsa.h"
#include "ecc_hamming.h"
#include "ecc_crc32.h"
#include "transport_esl.h"
#include "transport_hop.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_IDF_TARGET_ESP32
#include "esp_cpu.h" /* esp_cpu_get_cycle_count() (v6.x; ex esp_cpu_get_ccount) */
#define BENCH_HAVE_CCOUNT 1
#endif

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "BENCH-ICIT";

/* ARQ engine has no public header (declared in components/ecc/ecc_arq.c). */
extern CeePewErr_t ecc_arq_encode(const uint8_t *in, uint16_t in_len,
                                  uint8_t *out, uint16_t *out_len,
                                  uint16_t max_out_len);
extern CeePewErr_t ecc_arq_decode(const uint8_t *in, uint16_t in_len,
                                  uint8_t *out, uint16_t *out_len,
                                  uint16_t max_out_len, bool *corrected);
extern CeePewErr_t ecc_arq_reset(void);

/* ────────────────────────────────────────────────────────────────────── */
/* Constants                                                              */
/* ────────────────────────────────────────────────────────────────────── */

#define BENCH_N                200U
#define BENCH_N5                20U
#define BENCH_WD_YIELD_EVERY    20U

/* Worst-case 1-fragment chain sizes (bytes). */
#define BENCH_COMP_MAX         58U
#define BENCH_FRAG_LEN         60U
#define BENCH_ASCON_LEN        76U
#define BENCH_BOX_LEN          92U
#define BENCH_SIGNED_LEN      158U
#define BENCH_FECCRC_LEN      220U
#define BENCH_ESL_LEN         248U
#define BENCH_ARQ_LEN         249U

#ifdef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define BENCH_CPU_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#else
#define BENCH_CPU_MHZ 240
#endif

/* ────────────────────────────────────────────────────────────────────── */
/* Static storage (zero-heap: .bss only, no malloc)                       */
/* ────────────────────────────────────────────────────────────────────── */

/* 49B reference string: "CMD=OPEN_VALVE_04;AUTH=ADMIN;TEMP=26.4C;BAT=3.92V" */
static const uint8_t s_ref49[] =
    "CMD=OPEN_VALVE_04;AUTH=ADMIN;TEMP=26.4C;BAT=3.92V";
_Static_assert(sizeof(s_ref49) == 50U, "s_ref49 must be exactly 49 chars");

static uint8_t s_msg255[255];
static uint8_t s_comp49[CEEPEW_HUFF_BUF_MAX];
static uint16_t s_comp49_len = 0U;
static uint8_t s_decomp49[CEEPEW_MAX_MSG_BYTES];
static uint8_t s_comp255[CEEPEW_HUFF_BUF_MAX];
static uint16_t s_comp255_len = 0U;
static uint8_t s_decomp255[CEEPEW_MAX_MSG_BYTES];
static uint8_t s_comp58[BENCH_COMP_MAX];

static uint8_t s_priv_a[32];
static uint8_t s_pub_a[32];
static uint8_t s_priv_b[32];
static uint8_t s_pub_b[32];
static uint8_t s_ss[32];
static uint8_t s_dsum[32];
static uint8_t s_sha[32];
static uint8_t s_hkdf_ascon[16];
static uint8_t s_hkdf_box[16];
static uint8_t s_hkdf_pmk[16];
static uint8_t s_hkdf_lmk[16];
static uint8_t s_ascon_nonce[16];
static CryptoCtx_t s_bctx;
static uint8_t s_box_nonce[CRYPTO_BOX_NONCEBYTES];
static uint8_t s_eddsa_pk[32];
static uint8_t s_eddsa_sk[64];

static uint8_t s_frag[BENCH_FRAG_LEN];
static uint8_t s_ascon_ct[128];
static uint8_t s_box_ct[128];
static uint8_t s_sig[64];
static uint8_t s_frame158[BENCH_SIGNED_LEN];
static uint8_t s_fec_crc[CEEPEW_FEC_BUF_MAX];
static uint8_t s_esl[512];
static uint8_t s_arq[512];
static uint8_t s_work[512];
static uint8_t s_work2[512];
static uint8_t s_tmpa[160];
static uint8_t s_tmpb[160];
static uint8_t s_gold[512];
static uint16_t s_gold_len = 0U;
static uint8_t s_lite_gold[512];
static uint16_t s_lite_gold_len = 0U;
static uint16_t s_lite_ascon_len = 0U; /* Ascon ct len (FEC decode adds <=1 pad byte) */

static uint32_t s_t[BENCH_N];
static uint32_t s_c[BENCH_N];
static uint32_t s_sort[BENCH_N];

static const uint8_t s_peer_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
static uint64_t s_nc_base = 0U;
static bool s_esl_cb_by_bench = false;
static uint32_t s_worker_stack_hwm = 0U;

/* Core-1 worker (static allocation: zero-heap). */
#define BENCH_WORKER_STACK_WORDS (8192U / sizeof(StackType_t))
static StackType_t s_worker_stack[BENCH_WORKER_STACK_WORDS];
static StaticTask_t s_worker_tcb;
static TaskHandle_t s_main_task = NULL;
static volatile uint32_t s_worker_phase = 0U; /* 1 = TX+RX done, 2 = all done */

/* ────────────────────────────────────────────────────────────────────── */
/* Timing helpers                                                         */
/* ────────────────────────────────────────────────────────────────────── */

static inline uint32_t bench_ccount(void)
{
#ifdef BENCH_HAVE_CCOUNT
    return (uint32_t)esp_cpu_get_cycle_count();
#else
    return 0U;
#endif
}

static int64_t s_t0 = 0;
static uint32_t s_c0 = 0U;

static inline void bench_start(void)
{
    s_t0 = esp_timer_get_time();
    s_c0 = bench_ccount();
}

static inline void bench_stop(uint16_t i)
{
    uint32_t c1 = bench_ccount();
    int64_t t1 = esp_timer_get_time();
    s_c[i] = c1 - s_c0;
    s_t[i] = (uint32_t)(t1 - s_t0);
}

#define BENCH_YIELD(i) do {                                            \
    if ((((uint32_t)(i) + 1U) % BENCH_WD_YIELD_EVERY) == 0U) {          \
        vTaskDelay(1);                                                 \
    }                                                                  \
} while (0)

static uint32_t bench_isqrt_u64(uint64_t x)
{
    uint64_t r = 0U;
    uint64_t bit = (uint64_t)1 << 62;
    while (bit > x) { bit >>= 2; }
    while (bit != 0U) {
        uint64_t t = r + bit;
        r >>= 1;
        if (x >= t) { x -= t; r += bit; }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* Log one row: Mean/StdDev/p95/Cycles-per-Byte. All values measured. */
static void bench_report(const char *name, uint16_t n, uint16_t bytes, uint32_t fails)
{
    uint64_t sum = 0U;
    uint64_t csum = 0U;
    uint16_t i;
    for (i = 0U; i < n; i++) { sum += s_t[i]; csum += s_c[i]; }
    uint32_t mean = (uint32_t)(sum / (uint64_t)n);
    uint32_t cmean = (uint32_t)(csum / (uint64_t)n);

    uint64_t var = 0U;
    for (i = 0U; i < n; i++) {
        int64_t d = (int64_t)s_t[i] - (int64_t)mean;
        uint64_t sq = (uint64_t)(d * d);
        var += sq;
    }
    var /= (uint64_t)n;
    uint32_t sd = bench_isqrt_u64(var);

    for (i = 0U; i < n; i++) { s_sort[i] = s_t[i]; }
    for (i = 1U; i < n; i++) { /* insertion sort for p95 */
        uint32_t k = s_sort[i];
        uint16_t j = i;
        while (j > 0U && s_sort[j - 1U] > k) { s_sort[j] = s_sort[j - 1U]; j--; }
        s_sort[j] = k;
    }
    uint16_t p95_idx = (uint16_t)(((uint32_t)n * 95U) / 100U);
    if (p95_idx >= n) { p95_idx = (uint16_t)(n - 1U); }
    uint32_t p95 = s_sort[p95_idx];
    uint32_t cycB = (bytes != 0U) ? (cmean / (uint32_t)bytes) : 0U;

    ESP_LOGI(TAG, "[T] %-15s n=%u B=%u mean=%luus sd=%luus p95=%luus cyc=%lu cycB=%lu fail=%lu",
             name, (unsigned)n, (unsigned)bytes,
             (unsigned long)mean, (unsigned long)sd, (unsigned long)p95,
             (unsigned long)cmean, (unsigned long)cycB, (unsigned long)fails);
}

/* ────────────────────────────────────────────────────────────────────── */
/* ESL dummy callbacks (accept-all; benchmark only)                       */
/* ────────────────────────────────────────────────────────────────────── */

static CeePewErr_t bench_mac_ok(const uint8_t mac[6])
{
    (void)mac;
    return CEEPEW_OK;
}

static CeePewErr_t bench_nonce_ok(void)
{
    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────── */
/* One-shot chain helpers (shared by pre-pass + timed loops)              */
/* ────────────────────────────────────────────────────────────────────── */

/* Full TX chain: 58B comp -> frag -> ascon -> box -> sign -> fec+crc ->
 * ESL -> ARQ. Final frame lands in s_arq, length in *arq_len_out. */
static CeePewErr_t bench_tx_once(uint64_t nc, uint16_t *arq_len_out)
{
    CeePewErr_t err;
    uint16_t alen;
    uint16_t blen;
    uint16_t flen;
    uint32_t crc;
    uint16_t elen;
    uint16_t rlen;

    s_frag[0] = 0x80U; /* START bit + (total-1)=0 for 1-fragment message */
    s_frag[1] = 0U;
    memcpy(&s_frag[2], s_comp58, BENCH_COMP_MAX);

    alen = (uint16_t)sizeof(s_ascon_ct);
    err = crypto_ascon_aead_encrypt(s_hkdf_ascon, s_ascon_nonce, NULL, 0U,
                                    s_frag, BENCH_FRAG_LEN,
                                    s_ascon_ct, &alen);
    if (err != CEEPEW_OK) { return err; }

    blen = (uint16_t)sizeof(s_box_ct);
    err = crypto_box_encrypt(&s_bctx, s_pub_b, s_ascon_ct, alen,
                             s_box_ct, &blen);
    if (err != CEEPEW_OK) { return err; }

    err = crypto_eddsa_sign(s_eddsa_sk, s_box_ct, blen, s_sig);
    if (err != CEEPEW_OK) { return err; }

    s_frame158[0] = (uint8_t)(blen & 0xFFU);
    s_frame158[1] = (uint8_t)((blen >> 8U) & 0xFFU);
    memcpy(&s_frame158[2], s_box_ct, blen);
    memcpy(&s_frame158[2 + blen], s_sig, 64U);

    flen = (uint16_t)sizeof(s_fec_crc);
    err = ecc_hamming_encode(s_frame158, BENCH_SIGNED_LEN, s_fec_crc, &flen);
    if (err != CEEPEW_OK) { return err; }
    err = ecc_crc32_compute(s_fec_crc, flen, &crc);
    if (err != CEEPEW_OK) { return err; }
    memcpy(&s_fec_crc[flen], &crc, sizeof(crc));
    flen = (uint16_t)(flen + 4U);

    memcpy(s_esl, s_fec_crc, flen);
    elen = flen;
    err = transport_esl_process_outgoing(s_esl, &elen, (uint16_t)sizeof(s_esl), nc);
    if (err != CEEPEW_OK) { return err; }

    rlen = (uint16_t)sizeof(s_arq);
    err = ecc_arq_encode(s_esl, elen, s_arq, &rlen, (uint16_t)sizeof(s_arq));
    if (err != CEEPEW_OK) { return err; }

    *arq_len_out = rlen;
    return CEEPEW_OK;
}

/* Full RX chain (production order: ARQ -> ESL -> inner-CRC -> FEC ->
 * box -> ascon -> eddsa-verify -> 58B comp). Input frame is read-only. */
static CeePewErr_t bench_rx_once(const uint8_t *arq_in, uint16_t arq_len,
                                 uint8_t *comp_out58)
{
    CeePewErr_t err;
    uint16_t qlen;
    bool corr = false;
    uint16_t ilen;
    uint16_t dlen;
    bool fcorr = false;
    uint16_t box_ct_len;
    uint16_t alen;
    uint16_t plen;

    qlen = (uint16_t)sizeof(s_work);
    err = ecc_arq_decode(arq_in, arq_len, s_work, &qlen,
                         (uint16_t)sizeof(s_work), &corr);
    if (err != CEEPEW_OK) { return err; }

    ilen = qlen;
    err = transport_esl_process_incoming(s_work, &ilen, s_peer_mac, 0U);
    if (err != CEEPEW_OK) { return err; }

    err = ecc_crc32_verify(s_work, ilen);
    if (err != CEEPEW_OK) { return err; }

    dlen = (uint16_t)sizeof(s_work2);
    err = ecc_hamming_decode(s_work, (uint16_t)(ilen - 4U),
                             s_work2, &dlen, &fcorr);
    if (err != CEEPEW_OK) { return err; }

    box_ct_len = (uint16_t)((uint16_t)s_work2[0] | ((uint16_t)s_work2[1] << 8U));
    if ((uint16_t)(2U + box_ct_len + 64U) > dlen) { return CEEPEW_ERR_PARAM; }

    alen = (uint16_t)sizeof(s_tmpa);
    err = crypto_box_decrypt(&s_bctx, s_box_nonce, s_pub_b,
                             &s_work2[2], box_ct_len,
                             s_tmpa, &alen);
    if (err != CEEPEW_OK) { return err; }

    plen = (uint16_t)sizeof(s_tmpb);
    err = crypto_ascon_aead_decrypt(s_hkdf_ascon, s_ascon_nonce, NULL, 0U,
                                    s_tmpa, alen, s_tmpb, &plen);
    if (err != CEEPEW_OK) { return err; }

    err = crypto_eddsa_verify(s_eddsa_pk, &s_work2[2], box_ct_len,
                              &s_work2[2 + box_ct_len]);
    if (err != CEEPEW_OK) { return err; }

    if (plen != BENCH_FRAG_LEN) { return CEEPEW_ERR_PARAM; }
    memcpy(comp_out58, &s_tmpb[2], BENCH_COMP_MAX);
    return CEEPEW_OK;
}

/* Lite TX: frag -> ascon -> fec+crc -> ESL -> ARQ (skips box+eddsa). */
static CeePewErr_t bench_lite_tx_once(uint64_t nc, uint16_t *arq_len_out)
{
    CeePewErr_t err;
    uint16_t alen;
    uint16_t flen;
    uint32_t crc;
    uint16_t elen;
    uint16_t rlen;

    s_frag[0] = 0x80U;
    s_frag[1] = 0U;
    memcpy(&s_frag[2], s_comp58, BENCH_COMP_MAX);

    alen = (uint16_t)sizeof(s_ascon_ct);
    err = crypto_ascon_aead_encrypt(s_hkdf_ascon, s_ascon_nonce, NULL, 0U,
                                    s_frag, BENCH_FRAG_LEN,
                                    s_ascon_ct, &alen);
    if (err != CEEPEW_OK) { return err; }
    s_lite_ascon_len = alen;

    flen = (uint16_t)sizeof(s_fec_crc);
    err = ecc_hamming_encode(s_ascon_ct, alen, s_fec_crc, &flen);
    if (err != CEEPEW_OK) { return err; }
    err = ecc_crc32_compute(s_fec_crc, flen, &crc);
    if (err != CEEPEW_OK) { return err; }
    memcpy(&s_fec_crc[flen], &crc, sizeof(crc));
    flen = (uint16_t)(flen + 4U);

    memcpy(s_esl, s_fec_crc, flen);
    elen = flen;
    err = transport_esl_process_outgoing(s_esl, &elen, (uint16_t)sizeof(s_esl), nc);
    if (err != CEEPEW_OK) { return err; }

    rlen = (uint16_t)sizeof(s_arq);
    err = ecc_arq_encode(s_esl, elen, s_arq, &rlen, (uint16_t)sizeof(s_arq));
    if (err != CEEPEW_OK) { return err; }

    *arq_len_out = rlen;
    return CEEPEW_OK;
}

/* Lite RX: ARQ -> ESL -> inner-CRC -> FEC -> ascon -> 58B comp. */
static CeePewErr_t bench_lite_rx_once(const uint8_t *arq_in, uint16_t arq_len,
                                      uint8_t *comp_out58)
{
    CeePewErr_t err;
    uint16_t qlen;
    bool corr = false;
    uint16_t ilen;
    uint16_t dlen;
    bool fcorr = false;
    uint16_t plen;

    qlen = (uint16_t)sizeof(s_work);
    err = ecc_arq_decode(arq_in, arq_len, s_work, &qlen,
                         (uint16_t)sizeof(s_work), &corr);
    if (err != CEEPEW_OK) { return err; }

    ilen = qlen;
    err = transport_esl_process_incoming(s_work, &ilen, s_peer_mac, 0U);
    if (err != CEEPEW_OK) { return err; }

    err = ecc_crc32_verify(s_work, ilen);
    if (err != CEEPEW_OK) { return err; }

    dlen = (uint16_t)sizeof(s_work2);
    err = ecc_hamming_decode(s_work, (uint16_t)(ilen - 4U),
                             s_work2, &dlen, &fcorr);
    if (err != CEEPEW_OK) { return err; }

    /* Hamming(15,11) pads to a whole codeword: 76B (608b) -> 56cw -> 77B.
     * Strip the deterministic <=1 pad byte using the recorded ct length. */
    if (dlen != s_lite_ascon_len &&
        dlen != (uint16_t)(s_lite_ascon_len + 1U)) { return CEEPEW_ERR_PARAM; }

    plen = (uint16_t)sizeof(s_tmpb);
    err = crypto_ascon_aead_decrypt(s_hkdf_ascon, s_ascon_nonce, NULL, 0U,
                                    s_work2, s_lite_ascon_len, s_tmpb, &plen);
    if (err != CEEPEW_OK) { return err; }

    if (plen != BENCH_FRAG_LEN) { return CEEPEW_ERR_PARAM; }
    memcpy(comp_out58, &s_tmpb[2], BENCH_COMP_MAX);
    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Setup + correctness pre-pass (untimed, all sizes verified at runtime)  */
/* ────────────────────────────────────────────────────────────────────── */

static uint32_t bench_setup(void)
{
    uint32_t fails = 0U;
    CeePewErr_t err;
    uint16_t i;
    uint16_t olen;
    uint8_t ch;

    /* 255B ASCII pattern (deterministic). */
    {
        static const char pat[] = "VALVE_04 STATUS OK; TEMP=26.4C; ";
        uint16_t plen = (uint16_t)(sizeof(pat) - 1U);
        for (i = 0U; i < 255U; i++) { s_msg255[i] = (uint8_t)pat[i % plen]; }
    }

    /* Compress both messages. */
    olen = (uint16_t)sizeof(s_comp49);
    err = compress_huffman_compress(s_ref49, 49U, s_comp49, &olen,
                                    (uint16_t)sizeof(s_comp49), NULL);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "setup: comp49 failed %d", (int)err); fails++; }
    else { s_comp49_len = olen; }

    olen = (uint16_t)sizeof(s_comp255);
    err = compress_huffman_compress(s_msg255, 255U, s_comp255, &olen,
                                    (uint16_t)sizeof(s_comp255), NULL);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "setup: comp255 failed %d", (int)err); fails++; }
    else { s_comp255_len = olen; }

    /* Worst-case 58B compressed chunk: first 58B of real Huffman output
     * (opaque fragment payload for the crypto/FEC stages). */
    if (s_comp255_len >= BENCH_COMP_MAX) {
        memcpy(s_comp58, s_comp255, BENCH_COMP_MAX);
    } else {
        for (i = 0U; i < BENCH_COMP_MAX; i++) {
            s_comp58[i] = s_comp255[i % (s_comp255_len != 0U ? s_comp255_len : 1U)];
        }
    }
    ESP_LOGI(TAG, "setup: ref49=49B->comp=%uB; msg255=255B->comp=%uB; chain58=%uB",
             (unsigned)s_comp49_len, (unsigned)s_comp255_len, (unsigned)BENCH_COMP_MAX);

    /* X25519 fixtures (fixed scalars, clamped). */
    for (i = 0U; i < 32U; i++) {
        s_priv_a[i] = (uint8_t)(0x11U + i);
        s_priv_b[i] = (uint8_t)(0xA0U - i);
    }
    curve25519_clamp(s_priv_a);
    curve25519_clamp(s_priv_b);
    if (curve25519_scalarmult_base(s_pub_a, s_priv_a) != 0) { fails++; }
    if (curve25519_scalarmult_base(s_pub_b, s_priv_b) != 0) { fails++; }
    if (crypto_ecdh_reject_low_order(s_pub_a) != CEEPEW_OK) { fails++; }
    if (crypto_ecdh_reject_low_order(s_pub_b) != CEEPEW_OK) { fails++; }
    if (curve25519_scalarmult(s_ss, s_priv_a, s_pub_b) != 0) {
        ESP_LOGE(TAG, "setup: shared secret failed"); fails++;
    }

    /* Box context: local-only fixture (session stays inactive). */
    memset(&s_bctx, 0, sizeof(s_bctx));
    s_bctx.session_active = true;
    memcpy(s_bctx.box_privkey, s_priv_a, 32U);
    memcpy(s_bctx.box_pubkey, s_pub_a, 32U);
    memcpy(s_bctx.peer_box_pubkey, s_pub_b, 32U);
    s_bctx.peer_box_pubkey_valid = true;
    for (i = 0U; i < 16U; i++) { s_bctx.ascon_key[i] = (uint8_t)(0x33U + i); }
    for (i = 0U; i < 8U; i++) { s_bctx.session_id[i] = (uint8_t)(0x50U + i); }
    s_nc_base = session_get_nonce_counter();
    memcpy(s_box_nonce, s_bctx.session_id, 8U);
    for (i = 0U; i < 8U; i++) {
        s_box_nonce[8U + i] = (uint8_t)((s_nc_base >> (8U * i)) & 0xFFU);
    }
    memset(&s_box_nonce[16], 0, 8U);

    /* KDF chain fixtures. */
    digital_sum_mix(s_ref49, 49U, s_dsum);
    err = crypto_sha256_compute(s_ref49, 49U, s_sha);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "setup: sha256 failed"); fails++; }

    {
        static const char info_a[] = "CEEPEW-ascon-v1";
        static const char info_b[] = "CEEPEW-box-v1";
        static const char info_p[] = "CEEPEW-pmk-v1";
        static const char info_l[] = "CEEPEW-lmk-v1";
        static const uint8_t salt[16] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
        };
        if (crypto_hkdf_derive(s_ss, 32U, salt, 16U,
                               (const uint8_t *)info_a, (uint8_t)(sizeof(info_a) - 1U),
                               s_hkdf_ascon, 16U) != CEEPEW_OK) { fails++; }
        if (crypto_hkdf_derive(s_ss, 32U, salt, 16U,
                               (const uint8_t *)info_b, (uint8_t)(sizeof(info_b) - 1U),
                               s_hkdf_box, 16U) != CEEPEW_OK) { fails++; }
        if (crypto_hkdf_derive(s_ss, 32U, salt, 16U,
                               (const uint8_t *)info_p, (uint8_t)(sizeof(info_p) - 1U),
                               s_hkdf_pmk, 16U) != CEEPEW_OK) { fails++; }
        if (crypto_hkdf_derive(s_ss, 32U, salt, 16U,
                               (const uint8_t *)info_l, (uint8_t)(sizeof(info_l) - 1U),
                               s_hkdf_lmk, 16U) != CEEPEW_OK) { fails++; }
    }
    for (i = 0U; i < 16U; i++) { s_ascon_nonce[i] = (uint8_t)(0x77U + i); }

    /* Ed25519 (deterministic seeded setup; timed sign/verify is production path). */
    {
        static const uint8_t seed[32] = {
            0x9D, 0x61, 0xB1, 0x9D, 0xEF, 0xFD, 0x5A, 0x60,
            0xBA, 0x84, 0x4A, 0xF4, 0x92, 0xEC, 0x2C, 0xC4,
            0x44, 0x49, 0xC5, 0x69, 0x7B, 0x32, 0x69, 0x19,
            0x70, 0x3B, 0xAC, 0x03, 0x1C, 0xAE, 0x7F, 0x60
        };
        err = crypto_eddsa_seeded_keypair(s_eddsa_pk, s_eddsa_sk, seed);
        if (err != CEEPEW_OK) { ESP_LOGE(TAG, "setup: eddsa keypair failed"); fails++; }
    }

    /* FEC session. */
    err = ecc_hamming_init_session(s_hkdf_ascon);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "setup: fec init failed"); fails++; }

    /* ESL accept-all callbacks (only if nobody registered yet). */
    err = esl_register_callbacks(bench_mac_ok, bench_nonce_ok);
    if (err == CEEPEW_OK) { s_esl_cb_by_bench = true; }
    else { ESP_LOGW(TAG, "setup: ESL callbacks already registered (err=%d); incoming rows use existing path", (int)err); }
    transport_replay_reset();
    (void)ecc_arq_reset();

    /* Hop sanity (uses box ctx as CryptoCtx with ascon_key+session_id). */
    err = transport_get_current_channel(&s_bctx, s_nc_base, &ch);
    if (err != CEEPEW_OK || ch < 1U || ch > 9U) {
        ESP_LOGE(TAG, "setup: hop channel failed");
        fails++;
    }

    /* Full-chain round-trip pre-pass with runtime size verification. */
    {
        uint16_t rlen = 0U;
        uint8_t back58[BENCH_COMP_MAX];
        err = bench_tx_once(s_nc_base, &rlen);
        if (err != CEEPEW_OK) { ESP_LOGE(TAG, "setup: tx pre-pass failed %d", (int)err); fails++; }
        else {
            memcpy(s_gold, s_arq, rlen);
            s_gold_len = rlen;
            err = bench_rx_once(s_gold, s_gold_len, back58);
            if (err != CEEPEW_OK) { ESP_LOGE(TAG, "setup: rx pre-pass failed %d", (int)err); fails++; }
            else if (memcmp(back58, s_comp58, BENCH_COMP_MAX) != 0) {
                ESP_LOGE(TAG, "setup: round-trip mismatch"); fails++;
            }
        }
        ESP_LOGI(TAG, "setup: chain 58->60->76->92->158->220->248->249 pre-pass %s (arq=%uB)",
                 (fails == 0U) ? "OK" : "FAILED", (unsigned)rlen);
    }

    /* Lite pre-pass. */
    {
        uint16_t rlen = 0U;
        uint8_t back58[BENCH_COMP_MAX];
        err = bench_lite_tx_once(s_nc_base + 1000U, &rlen);
        if (err != CEEPEW_OK) { ESP_LOGE(TAG, "setup: lite tx pre-pass failed %d", (int)err); fails++; }
        else {
            memcpy(s_lite_gold, s_arq, rlen);
            s_lite_gold_len = rlen;
            (void)ecc_arq_reset();
            transport_replay_reset();
            err = bench_lite_rx_once(s_lite_gold, s_lite_gold_len, back58);
            if (err != CEEPEW_OK) { ESP_LOGE(TAG, "setup: lite rx pre-pass failed %d", (int)err); fails++; }
            else if (memcmp(back58, s_comp58, BENCH_COMP_MAX) != 0) {
                ESP_LOGE(TAG, "setup: lite round-trip mismatch"); fails++;
            }
        }
        ESP_LOGI(TAG, "setup: lite chain pre-pass %s (arq=%uB)",
                 (fails == 0U) ? "OK" : "FAILED", (unsigned)rlen);
    }

    (void)ecc_arq_reset();
    transport_replay_reset();
    return fails;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Per-stage timed loops (run in integration task context)                */
/* ────────────────────────────────────────────────────────────────────── */

static void bench_stage_compress(void)
{
    uint32_t fails = 0U;
    uint16_t i;
    uint16_t olen;
    for (i = 0U; i < BENCH_N; i++) {
        olen = (uint16_t)sizeof(s_comp49);
        bench_start();
        CeePewErr_t e = compress_huffman_compress(s_ref49, 49U, s_comp49, &olen,
                                                  (uint16_t)sizeof(s_comp49), NULL);
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("huff_c49", BENCH_N, 49U, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        olen = (uint16_t)sizeof(s_decomp49);
        bench_start();
        CeePewErr_t e = compress_huffman_decompress(s_comp49, s_comp49_len,
                                                    s_decomp49, &olen,
                                                    (uint16_t)sizeof(s_decomp49));
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("huff_d49", BENCH_N, s_comp49_len, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        olen = (uint16_t)sizeof(s_comp255);
        bench_start();
        CeePewErr_t e = compress_huffman_compress(s_msg255, 255U, s_comp255, &olen,
                                                  (uint16_t)sizeof(s_comp255), NULL);
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("huff_c255", BENCH_N, 255U, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        olen = (uint16_t)sizeof(s_decomp255);
        bench_start();
        CeePewErr_t e = compress_huffman_decompress(s_comp255, s_comp255_len,
                                                    s_decomp255, &olen,
                                                    (uint16_t)sizeof(s_decomp255));
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("huff_d255", BENCH_N, s_comp255_len, fails);
}

static void bench_stage_ecdh(void)
{
    uint32_t fails = 0U;
    uint16_t i;
    uint8_t q[32];
    for (i = 0U; i < BENCH_N; i++) {
        bench_start();
        int rc = curve25519_scalarmult_base(q, s_priv_a);
        bench_stop(i);
        if (rc != 0) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("x25519_base", BENCH_N, 32U, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        bench_start();
        int rc = curve25519_scalarmult(q, s_priv_a, s_pub_b);
        bench_stop(i);
        if (rc != 0) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("x25519_mul", BENCH_N, 32U, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        bench_start();
        CeePewErr_t e = crypto_ecdh_reject_low_order(s_pub_b);
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("ecdh_loword", BENCH_N, 32U, fails);
}

static void bench_stage_kdf(void)
{
    uint32_t fails = 0U;
    uint16_t i;
    for (i = 0U; i < BENCH_N; i++) {
        bench_start();
        digital_sum_mix(s_ref49, 49U, s_dsum);
        bench_stop(i);
        BENCH_YIELD(i);
    }
    bench_report("dsum_mix", BENCH_N, 49U, fails);

    for (i = 0U; i < BENCH_N; i++) {
        bench_start();
        CeePewErr_t e = crypto_sha256_compute(s_ref49, 49U, s_sha);
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("sha256", BENCH_N, 49U, fails);

    fails = 0U;
    {
        static const uint8_t salt[16] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
        };
        static const char ia[] = "CEEPEW-ascon-v1";
        static const char ib[] = "CEEPEW-box-v1";
        static const char ip[] = "CEEPEW-pmk-v1";
        static const char il[] = "CEEPEW-lmk-v1";
        for (i = 0U; i < BENCH_N; i++) {
            bench_start();
            CeePewErr_t e = crypto_hkdf_derive(s_ss, 32U, salt, 16U,
                                               (const uint8_t *)ia, (uint8_t)(sizeof(ia) - 1U),
                                               s_hkdf_ascon, 16U);
            if (e == CEEPEW_OK) {
                e = crypto_hkdf_derive(s_ss, 32U, salt, 16U,
                                       (const uint8_t *)ib, (uint8_t)(sizeof(ib) - 1U),
                                       s_hkdf_box, 16U);
            }
            if (e == CEEPEW_OK) {
                e = crypto_hkdf_derive(s_ss, 32U, salt, 16U,
                                       (const uint8_t *)ip, (uint8_t)(sizeof(ip) - 1U),
                                       s_hkdf_pmk, 16U);
            }
            if (e == CEEPEW_OK) {
                e = crypto_hkdf_derive(s_ss, 32U, salt, 16U,
                                       (const uint8_t *)il, (uint8_t)(sizeof(il) - 1U),
                                       s_hkdf_lmk, 16U);
            }
            bench_stop(i);
            if (e != CEEPEW_OK) { fails++; }
            BENCH_YIELD(i);
        }
    }
    bench_report("hkdf_x4", BENCH_N, 64U, fails);
}

static void bench_stage_aead(void)
{
    uint32_t fails = 0U;
    uint16_t i;
    uint16_t olen;

    s_frag[0] = 0x80U;
    s_frag[1] = 0U;
    memcpy(&s_frag[2], s_comp58, BENCH_COMP_MAX);

    for (i = 0U; i < BENCH_N; i++) {
        olen = (uint16_t)sizeof(s_ascon_ct);
        bench_start();
        CeePewErr_t e = crypto_ascon_aead_encrypt(s_hkdf_ascon, s_ascon_nonce, NULL, 0U,
                                                  s_frag, BENCH_FRAG_LEN,
                                                  s_ascon_ct, &olen);
        bench_stop(i);
        if (e != CEEPEW_OK || olen != BENCH_ASCON_LEN) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("ascon_enc", BENCH_N, BENCH_FRAG_LEN, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        olen = (uint16_t)sizeof(s_tmpb);
        bench_start();
        CeePewErr_t e = crypto_ascon_aead_decrypt(s_hkdf_ascon, s_ascon_nonce, NULL, 0U,
                                                  s_ascon_ct, BENCH_ASCON_LEN,
                                                  s_tmpb, &olen);
        bench_stop(i);
        if (e != CEEPEW_OK || olen != BENCH_FRAG_LEN) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("ascon_dec", BENCH_N, BENCH_ASCON_LEN, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        olen = (uint16_t)sizeof(s_box_ct);
        bench_start();
        CeePewErr_t e = crypto_box_encrypt(&s_bctx, s_pub_b, s_ascon_ct,
                                           BENCH_ASCON_LEN, s_box_ct, &olen);
        bench_stop(i);
        if (e != CEEPEW_OK || olen != BENCH_BOX_LEN) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("box_enc", BENCH_N, BENCH_ASCON_LEN, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        olen = (uint16_t)sizeof(s_tmpa);
        bench_start();
        CeePewErr_t e = crypto_box_decrypt(&s_bctx, s_box_nonce, s_pub_b,
                                           s_box_ct, BENCH_BOX_LEN,
                                           s_tmpa, &olen);
        bench_stop(i);
        if (e != CEEPEW_OK || olen != BENCH_ASCON_LEN) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("box_dec", BENCH_N, BENCH_BOX_LEN, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        bench_start();
        CeePewErr_t e = crypto_eddsa_sign(s_eddsa_sk, s_box_ct, BENCH_BOX_LEN, s_sig);
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("eddsa_sign", BENCH_N, BENCH_BOX_LEN, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        bench_start();
        CeePewErr_t e = crypto_eddsa_verify(s_eddsa_pk, s_box_ct, BENCH_BOX_LEN, s_sig);
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("eddsa_verify", BENCH_N, BENCH_BOX_LEN, fails);
}

static void bench_stage_fec_crc(void)
{
    uint32_t fails = 0U;
    uint16_t i;
    uint16_t olen;
    uint32_t crc;
    bool corrected;

    /* Build the 158B signed frame fixture. */
    s_frame158[0] = (uint8_t)(BENCH_BOX_LEN & 0xFFU);
    s_frame158[1] = (uint8_t)((BENCH_BOX_LEN >> 8U) & 0xFFU);
    memcpy(&s_frame158[2], s_box_ct, BENCH_BOX_LEN);
    memcpy(&s_frame158[2 + BENCH_BOX_LEN], s_sig, 64U);

    for (i = 0U; i < BENCH_N; i++) {
        olen = (uint16_t)sizeof(s_fec_crc);
        bench_start();
        CeePewErr_t e = ecc_hamming_encode(s_frame158, BENCH_SIGNED_LEN,
                                           s_fec_crc, &olen);
        if (e == CEEPEW_OK) { e = ecc_crc32_compute(s_fec_crc, olen, &crc); }
        if (e == CEEPEW_OK) {
            memcpy(&s_fec_crc[olen], &crc, sizeof(crc));
            olen = (uint16_t)(olen + 4U);
        }
        bench_stop(i);
        if (e != CEEPEW_OK || olen != BENCH_FECCRC_LEN) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("fec_enc", BENCH_N, BENCH_SIGNED_LEN, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        bench_start();
        CeePewErr_t e = ecc_crc32_verify(s_fec_crc, BENCH_FECCRC_LEN);
        if (e == CEEPEW_OK) {
            olen = (uint16_t)sizeof(s_work2);
            corrected = false;
            e = ecc_hamming_decode(s_fec_crc, BENCH_FECCRC_LEN - 4U,
                                   s_work2, &olen, &corrected);
        }
        bench_stop(i);
        if (e != CEEPEW_OK || corrected) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("fec_dec_clean", BENCH_N, BENCH_FECCRC_LEN, fails);

    /* 1-bit flip per codeword: bit (15k) of the FEC stream for k=0..cw-1. */
    fails = 0U;
    {
        uint16_t fec_len = (uint16_t)(BENCH_FECCRC_LEN - 4U);
        uint16_t cw = (uint16_t)(((uint32_t)fec_len * 8U) / 15U);
        for (i = 0U; i < BENCH_N; i++) {
            uint16_t k;
            memcpy(s_work, s_fec_crc, BENCH_FECCRC_LEN);
            for (k = 0U; k < cw; k++) {
                uint32_t bitpos = (uint32_t)k * 15U;
                s_work[bitpos / 8U] ^= (uint8_t)(1U << (bitpos % 8U));
            }
            bench_start();
            olen = (uint16_t)sizeof(s_work2);
            corrected = false;
            CeePewErr_t e = ecc_hamming_decode(s_work, fec_len,
                                               s_work2, &olen, &corrected);
            bench_stop(i);
            if (e != CEEPEW_OK || !corrected) { fails++; }
            BENCH_YIELD(i);
        }
    }
    bench_report("fec_dec_1bit", BENCH_N, BENCH_FECCRC_LEN, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        bench_start();
        CeePewErr_t e = ecc_crc32_compute(s_fec_crc, BENCH_FECCRC_LEN, &crc);
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("crc32_compute", BENCH_N, BENCH_FECCRC_LEN, fails);

    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        bench_start();
        CeePewErr_t e = ecc_crc32_verify(s_fec_crc, BENCH_FECCRC_LEN);
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("crc32_verify", BENCH_N, BENCH_FECCRC_LEN, fails);
}

static void bench_stage_esl(void)
{
    uint32_t fails = 0U;
    uint16_t i;
    uint16_t olen;
    uint16_t ilen;

    /* ESL wrap: 220B -> 248B. */
    for (i = 0U; i < BENCH_N; i++) {
        memcpy(s_work, s_fec_crc, BENCH_FECCRC_LEN);
        olen = BENCH_FECCRC_LEN;
        bench_start();
        CeePewErr_t e = transport_esl_process_outgoing(s_work, &olen,
                                                       (uint16_t)sizeof(s_work),
                                                       s_nc_base + (uint64_t)i);
        bench_stop(i);
        if (e != CEEPEW_OK || olen != BENCH_ESL_LEN) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("esl_wrap", BENCH_N, BENCH_FECCRC_LEN, fails);

    /* ESL incoming, legit path: fresh frame per iteration (outgoing untimed). */
    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        memcpy(s_work, s_fec_crc, BENCH_FECCRC_LEN);
        olen = BENCH_FECCRC_LEN;
        (void)transport_esl_process_outgoing(s_work, &olen,
                                             (uint16_t)sizeof(s_work),
                                             s_nc_base + 100000ULL + (uint64_t)i);
        ilen = olen;
        bench_start();
        CeePewErr_t e = transport_esl_process_incoming(s_work, &ilen, s_peer_mac, 0U);
        bench_stop(i);
        if (e != CEEPEW_OK || ilen != BENCH_FECCRC_LEN) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("esl_in_legit", BENCH_N, BENCH_ESL_LEN, fails);

    /* ESL incoming, replayed seq: same frame re-fed (reject at replay stage). */
    fails = 0U;
    {
        uint16_t glen;
        memcpy(s_work, s_fec_crc, BENCH_FECCRC_LEN);
        glen = BENCH_FECCRC_LEN;
        (void)transport_esl_process_outgoing(s_work, &glen,
                                             (uint16_t)sizeof(s_work),
                                             s_nc_base + 200000ULL);
        memcpy(s_gold, s_work, glen);
        s_gold_len = glen;
        ilen = glen; /* pre-pass accept (untimed) */
        memcpy(s_work, s_gold, s_gold_len);
        if (transport_esl_process_incoming(s_work, &ilen, s_peer_mac, 0U) != CEEPEW_OK) {
            ESP_LOGW(TAG, "esl replay pre-pass not accepted; replay row measures first-reject path");
        }
        for (i = 0U; i < BENCH_N; i++) {
            memcpy(s_work, s_gold, s_gold_len);
            ilen = s_gold_len;
            bench_start();
            CeePewErr_t e = transport_esl_process_incoming(s_work, &ilen, s_peer_mac, 0U);
            bench_stop(i);
            if (e == CEEPEW_OK) { fails++; } /* replays must reject */
            BENCH_YIELD(i);
        }
    }
    bench_report("esl_in_replay", BENCH_N, BENCH_ESL_LEN, fails);

    /* ESL incoming, bad CRC: payload byte flipped (reject at CRC stage). */
    fails = 0U;
    {
        uint16_t glen;
        memcpy(s_work, s_fec_crc, BENCH_FECCRC_LEN);
        glen = BENCH_FECCRC_LEN;
        (void)transport_esl_process_outgoing(s_work, &glen,
                                             (uint16_t)sizeof(s_work),
                                             s_nc_base + 300000ULL);
        memcpy(s_gold, s_work, glen);
        s_gold_len = glen;
        for (i = 0U; i < BENCH_N; i++) {
            memcpy(s_work, s_gold, s_gold_len);
            s_work[30] ^= 0x01U; /* inside payload, CRC now stale */
            ilen = s_gold_len;
            bench_start();
            CeePewErr_t e = transport_esl_process_incoming(s_work, &ilen, s_peer_mac, 0U);
            bench_stop(i);
            if (e == CEEPEW_OK) { fails++; } /* corrupt frames must reject */
            BENCH_YIELD(i);
        }
    }
    bench_report("esl_in_badcrc", BENCH_N, BENCH_ESL_LEN, fails);
}

static void bench_stage_hop_arq(void)
{
    uint32_t fails = 0U;
    uint16_t i;
    uint8_t ch;
    uint16_t olen;
    bool corrected;

    for (i = 0U; i < BENCH_N; i++) {
        bench_start();
        CeePewErr_t e = transport_get_current_channel(&s_bctx,
                                                      s_nc_base + (uint64_t)i,
                                                      &ch);
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("hop_channel", BENCH_N, 24U, fails);

    (void)ecc_arq_reset();
    fails = 0U;
    for (i = 0U; i < BENCH_N; i++) {
        olen = (uint16_t)sizeof(s_arq);
        bench_start();
        CeePewErr_t e = ecc_arq_encode(s_esl, BENCH_ESL_LEN, s_arq, &olen,
                                       (uint16_t)sizeof(s_arq));
        bench_stop(i);
        if (e != CEEPEW_OK || olen != BENCH_ARQ_LEN) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("arq_encode", BENCH_N, BENCH_ESL_LEN, fails);

    /* NOTE: s_esl currently holds a 220B FEC payload, not a 248B ESL frame;
     * rebuild a proper golden ESL frame for the decode row. */
    fails = 0U;
    {
        uint16_t glen;
        memcpy(s_work, s_fec_crc, BENCH_FECCRC_LEN);
        glen = BENCH_FECCRC_LEN;
        (void)transport_esl_process_outgoing(s_work, &glen,
                                             (uint16_t)sizeof(s_work),
                                             s_nc_base + 400000ULL);
        memcpy(s_esl, s_work, glen);
        (void)ecc_arq_reset();
        for (i = 0U; i < BENCH_N; i++) {
            olen = (uint16_t)sizeof(s_arq);
            (void)ecc_arq_encode(s_esl, BENCH_ESL_LEN, s_arq, &olen,
                                 (uint16_t)sizeof(s_arq));
            bench_start();
            uint16_t dlen = (uint16_t)sizeof(s_work);
            corrected = false;
            CeePewErr_t e = ecc_arq_decode(s_arq, olen, s_work, &dlen,
                                           (uint16_t)sizeof(s_work), &corrected);
            bench_stop(i);
            if (e != CEEPEW_OK || dlen != BENCH_ESL_LEN) { fails++; }
            BENCH_YIELD(i);
        }
    }
    bench_report("arq_decode", BENCH_N, BENCH_ARQ_LEN, fails);
    (void)ecc_arq_reset();
}

/* ────────────────────────────────────────────────────────────────────── */
/* Totals (run on Core 1 via static worker; fallback: inline)             */
/* ────────────────────────────────────────────────────────────────────── */

static uint32_t bench_run_tx_total(uint16_t n)
{
    uint32_t fails = 0U;
    uint16_t i;
    for (i = 0U; i < n; i++) {
        uint16_t rlen = 0U;
        bench_start();
        CeePewErr_t e = bench_tx_once(s_nc_base + 500000ULL + (uint64_t)i, &rlen);
        bench_stop(i);
        if (e != CEEPEW_OK || rlen != BENCH_ARQ_LEN) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("tx_total", n, BENCH_COMP_MAX, fails);
    return fails;
}

static uint32_t bench_run_rx_total(uint16_t n)
{
    uint32_t fails = 0U;
    uint16_t i;
    uint8_t back58[BENCH_COMP_MAX];

    /* Fresh golden frame (timestamp slack is 45 s). */
    {
        uint16_t rlen = 0U;
        (void)bench_tx_once(s_nc_base + 600000ULL, &rlen);
        memcpy(s_gold, s_arq, rlen);
        s_gold_len = rlen;
    }
    for (i = 0U; i < n; i++) {
        /* Same-frame replays would reject: reset replay+ARQ state (untimed). */
        transport_replay_reset();
        (void)ecc_arq_reset();
        bench_start();
        CeePewErr_t e = bench_rx_once(s_gold, s_gold_len, back58);
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("rx_total", n, BENCH_ARQ_LEN, fails);
    (void)ecc_arq_reset();
    transport_replay_reset();
    return fails;
}

static uint32_t bench_run_lite_totals(uint16_t n)
{
    uint32_t fails = 0U;
    uint16_t i;

    for (i = 0U; i < n; i++) {
        uint16_t rlen = 0U;
        bench_start();
        CeePewErr_t e = bench_lite_tx_once(s_nc_base + 700000ULL + (uint64_t)i, &rlen);
        bench_stop(i);
        if (e != CEEPEW_OK) { fails++; }
        BENCH_YIELD(i);
    }
    bench_report("lite_tx", n, BENCH_FRAG_LEN, fails);

    {
        uint16_t rlen = 0U;
        (void)bench_lite_tx_once(s_nc_base + 800000ULL, &rlen);
        memcpy(s_lite_gold, s_arq, rlen);
        s_lite_gold_len = rlen;
    }
    fails = 0U;
    {
        uint8_t back58[BENCH_COMP_MAX];
        for (i = 0U; i < n; i++) {
            transport_replay_reset();
            (void)ecc_arq_reset();
            bench_start();
            CeePewErr_t e = bench_lite_rx_once(s_lite_gold, s_lite_gold_len, back58);
            bench_stop(i);
            if (e != CEEPEW_OK) { fails++; }
            BENCH_YIELD(i);
        }
    }
    bench_report("lite_rx", n, s_lite_gold_len, fails);
    (void)ecc_arq_reset();
    transport_replay_reset();
    return fails;
}

static void bench_worker_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "worker: running totals on core %d", (int)xPortGetCoreID());
    bench_run_tx_total(BENCH_N);
    bench_run_rx_total(BENCH_N);
    s_worker_phase = 1U;
    xTaskNotifyGive(s_main_task);
    bench_run_lite_totals(BENCH_N);
    s_worker_stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    s_worker_phase = 2U;
    xTaskNotifyGive(s_main_task);
    vTaskDelete(NULL);
}

/* 5-fragment (255B-class) TX chain for region-usage comparison. */
static void bench_5frag(uint32_t *hwm_before, uint32_t *hwm_after)
{
    uint32_t fails = 0U;
    uint16_t i;
    uint8_t k;
    *hwm_before = g_region.hwm;
    for (i = 0U; i < BENCH_N5; i++) {
        bench_start();
        for (k = 0U; k < 5U; k++) {
            CeePewErr_t e;
            uint16_t alen;
            uint16_t blen;
            uint16_t flen;
            uint32_t crc;
            uint16_t elen;
            uint16_t rlen;
            s_frag[0] = (uint8_t)((k == 0U ? 0x80U : 0x00U) | 0x04U);
            s_frag[1] = k;
            memcpy(&s_frag[2], s_comp58, BENCH_COMP_MAX);
            s_frag[2] ^= k; /* per-fragment variation, same worst-case size */
            alen = (uint16_t)sizeof(s_ascon_ct);
            e = crypto_ascon_aead_encrypt(s_hkdf_ascon, s_ascon_nonce, NULL, 0U,
                                          s_frag, BENCH_FRAG_LEN,
                                          s_ascon_ct, &alen);
            if (e == CEEPEW_OK) {
                blen = (uint16_t)sizeof(s_box_ct);
                e = crypto_box_encrypt(&s_bctx, s_pub_b, s_ascon_ct, alen,
                                       s_box_ct, &blen);
            }
            if (e == CEEPEW_OK) { e = crypto_eddsa_sign(s_eddsa_sk, s_box_ct, blen, s_sig); }
            if (e == CEEPEW_OK) {
                s_frame158[0] = (uint8_t)(blen & 0xFFU);
                s_frame158[1] = (uint8_t)((blen >> 8U) & 0xFFU);
                memcpy(&s_frame158[2], s_box_ct, blen);
                memcpy(&s_frame158[2 + blen], s_sig, 64U);
                flen = (uint16_t)sizeof(s_fec_crc);
                e = ecc_hamming_encode(s_frame158, BENCH_SIGNED_LEN, s_fec_crc, &flen);
            }
            if (e == CEEPEW_OK) { e = ecc_crc32_compute(s_fec_crc, flen, &crc); }
            if (e == CEEPEW_OK) {
                memcpy(&s_fec_crc[flen], &crc, sizeof(crc));
                flen = (uint16_t)(flen + 4U);
                memcpy(s_esl, s_fec_crc, flen);
                elen = flen;
                e = transport_esl_process_outgoing(s_esl, &elen,
                                                   (uint16_t)sizeof(s_esl),
                                                   s_nc_base + 900000ULL + (uint64_t)i * 5ULL + k);
            }
            if (e == CEEPEW_OK) {
                rlen = (uint16_t)sizeof(s_arq);
                e = ecc_arq_encode(s_esl, elen, s_arq, &rlen, (uint16_t)sizeof(s_arq));
            }
            if (e != CEEPEW_OK) { fails++; break; }
        }
        bench_stop(i);
        BENCH_YIELD(i);
    }
    *hwm_after = g_region.hwm;
    bench_report("tx_total_5frag", BENCH_N5, 5U * BENCH_COMP_MAX, fails);
}

/* ────────────────────────────────────────────────────────────────────── */
/* Entry point                                                            */
/* ────────────────────────────────────────────────────────────────────── */

void test_icitiit_paper_benchmarks(void)
{
    uint32_t setup_fails = 0U;
    uint32_t heap_free_before = 0U;
    uint32_t heap_min_before = 0U;
    uint32_t heap_free_after = 0U;
    uint32_t heap_min_after = 0U;
    uint32_t hwm_pre = 0U;
    uint32_t hwm_post_stages = 0U;
    uint32_t hwm_post_totals = 0U;
    uint32_t hwm_5_before = 0U;
    uint32_t hwm_5_after = 0U;
    uint32_t main_stack_hwm = 0U;
    int main_core;

    main_core = (int)xPortGetCoreID();
    ESP_LOGI(TAG, "=== CEE-PEW ON-DEVICE STAGE TIMINGS ===");
    ESP_LOGI(TAG, "cpu=Xtensa LX6 @%dMHz n=%u ccount=%s core=%d",
             (int)BENCH_CPU_MHZ, (unsigned)BENCH_N,
#ifdef BENCH_HAVE_CCOUNT
             "esp_cpu_get_cycle_count",
#else
             "N/A",
#endif
             main_core);

    heap_free_before = (uint32_t)esp_get_free_heap_size();
    heap_min_before = (uint32_t)esp_get_minimum_free_heap_size();
    hwm_pre = g_region.hwm;

    setup_fails = bench_setup();
    ESP_LOGI(TAG, "setup fails=%lu", (unsigned long)setup_fails);

    bench_stage_compress();
    bench_stage_ecdh();
    bench_stage_kdf();
    bench_stage_aead();
    bench_stage_fec_crc();
    bench_stage_esl();
    bench_stage_hop_arq();
    hwm_post_stages = g_region.hwm;

    /* Full TX/RX totals on Core 1 (static worker; inline fallback). */
    s_main_task = xTaskGetCurrentTaskHandle();
    s_worker_phase = 0U;
    {
        TaskHandle_t w = xTaskCreateStaticPinnedToCore(
            bench_worker_fn, "bench_icitiit",
            BENCH_WORKER_STACK_WORDS, NULL,
            tskIDLE_PRIORITY + 1, s_worker_stack, &s_worker_tcb, 1);
        if (w == NULL) {
            ESP_LOGW(TAG, "worker create failed; running totals inline on core %d", main_core);
            bench_run_tx_total(BENCH_N);
            bench_run_rx_total(BENCH_N);
            heap_free_after = (uint32_t)esp_get_free_heap_size();
            heap_min_after = (uint32_t)esp_get_minimum_free_heap_size();
            hwm_post_totals = g_region.hwm;
            bench_run_lite_totals(BENCH_N);
        } else {
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(180000)) == 0U || s_worker_phase < 1U) {
                ESP_LOGE(TAG, "worker phase1 timeout; results incomplete");
            }
            /* Heap delta across 200x (TX+RX). Minimum-free is monotonic. */
            heap_free_after = (uint32_t)esp_get_free_heap_size();
            heap_min_after = (uint32_t)esp_get_minimum_free_heap_size();
            hwm_post_totals = g_region.hwm;
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(180000)) == 0U || s_worker_phase < 2U) {
                ESP_LOGE(TAG, "worker phase2 timeout; lite results incomplete");
            }
        }
    }
    if (s_worker_phase < 1U && heap_free_after == 0U) {
        heap_free_after = (uint32_t)esp_get_free_heap_size();
        heap_min_after = (uint32_t)esp_get_minimum_free_heap_size();
    }

    ESP_LOGI(TAG, "[H] heap before: free=%luB min=%luB",
             (unsigned long)heap_free_before, (unsigned long)heap_min_before);
    ESP_LOGI(TAG, "[H] heap after 200xTX+RX: free=%luB min=%luB delta_free=%ldB",
             (unsigned long)heap_free_after, (unsigned long)heap_min_after,
             (long)((int64_t)heap_free_after - (int64_t)heap_free_before));
    ESP_LOGI(TAG, "[R] 1-frag: hwm_pre=%lu hwm_post_stages=%lu hwm_post_totals=%lu usage_pct=%u%%",
             (unsigned long)hwm_pre, (unsigned long)hwm_post_stages,
             (unsigned long)hwm_post_totals, (unsigned)region_usage_pct(&g_region));

    /* 5-fragment region comparison (255B class). */
    bench_5frag(&hwm_5_before, &hwm_5_after);
    ESP_LOGI(TAG, "[R] 5-frag: hwm_before=%lu hwm_after=%lu delta=%ld usage_pct=%u%%",
             (unsigned long)hwm_5_before, (unsigned long)hwm_5_after,
             (long)((int64_t)hwm_5_after - (int64_t)hwm_5_before),
             (unsigned)region_usage_pct(&g_region));

    /* Stack + sizes. */
    main_stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "[S] stack_hwm main=%luB worker=%luB sizeof(CryptoCtx)=%u sizeof(Region)=%u arq_buf=%u worker_stack=%u",
             (unsigned long)main_stack_hwm, (unsigned long)s_worker_stack_hwm,
             (unsigned)sizeof(CryptoCtx_t), (unsigned)sizeof(Region_t),
             (unsigned)sizeof(s_arq),
             (unsigned)sizeof(s_worker_stack));

    /* Restore global state touched by the benchmark. */
    (void)ecc_arq_reset();
    transport_replay_reset();
    (void)ecc_hamming_deinit();
    if (s_esl_cb_by_bench) {
        esl_reset_callbacks();
        s_esl_cb_by_bench = false;
    }
    ceepew_secure_zero(s_priv_a, sizeof(s_priv_a));
    ceepew_secure_zero(s_priv_b, sizeof(s_priv_b));
    ceepew_secure_zero(s_ss, sizeof(s_ss));
    ceepew_secure_zero(s_hkdf_ascon, sizeof(s_hkdf_ascon));
    ceepew_secure_zero(s_hkdf_box, sizeof(s_hkdf_box));
    ceepew_secure_zero(s_hkdf_pmk, sizeof(s_hkdf_pmk));
    ceepew_secure_zero(s_hkdf_lmk, sizeof(s_hkdf_lmk));
    ceepew_secure_zero(&s_bctx, sizeof(s_bctx));
    ceepew_secure_zero(s_eddsa_sk, sizeof(s_eddsa_sk));
    ESP_LOGI(TAG, "=== CEE-PEW BENCHMARK DONE ===");
}

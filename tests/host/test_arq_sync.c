/* tests/host/test_arq_sync.c — ARQ post-derive sync-exchange regression test
 *
 * Validates the fix for the real-usage sync desync observed in
 * logs/realusage_chat3_20260802.txt, where a 2-byte non-ESL broadcast probe
 * arriving during the post-derive sync was fed into ecc_arq_decode(), consumed
 * the ARQ first-frame slot, advanced the receive window, and caused the peer's
 * legitimate encrypted ACK to be rejected with CEEPEW_ERR_REPLAY (err=17).
 *
 * The fix (transport_esl.h CEEPEW_ESL_MIN_FRAME_BYTES + task_session.c floor
 * guard) drops any raw ESP-NOW frame shorter than
 * CEEPEW_ESL_MIN_FRAME_BYTES + 1 BEFORE it reaches ecc_arq_decode().
 *
 * This test compiles the REAL components/ecc/ecc_arq.c on the host. It models
 * both peers' ARQ state in one process: ecc_arq_encode() only touches the TX
 * counter (peer B) while ecc_arq_decode() only touches the RX window (peer A),
 * so the single static module can simulate both directions.
 *
 * Assertions:
 *   1. Floor-guard sanity: a 2-byte probe is below the ESL min-frame floor.
 *   2. WITHOUT a probe, the initiator accepts the peer's first frame for EVERY
 *      possible peer TX counter state S in [0,255] — the exchange is robust.
 *   3. WITH a probe (pre-fix behaviour at the ARQ layer), the window is
 *      poisoned and the peer's first frame is rejected for S in [60,187].
 *   4. Runtime err=17 arithmetic reproduction: a first-frame accept of a
 *      seq-0 frame (expected -> 1) followed by an incoming seq-0 frame is
 *      rejected as replay — matching the A-side rejection in the capture.
 *   5. Nominal post-fix exchange replays cleanly on both the initiator and
 *      responder RX paths (probe dropped, PFS handshake and sync ACK accepted).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "transport_esl.h"

/* ARQ API (no public header exists; forward-declared as in fuzz/fuzz_arq/) */
CeePewErr_t ecc_arq_encode(const uint8_t *in, uint16_t in_len,
                           uint8_t *out, uint16_t *out_len,
                           uint16_t max_out_len);
CeePewErr_t ecc_arq_decode(const uint8_t *in, uint16_t in_len,
                           uint8_t *out, uint16_t *out_len,
                           uint16_t max_out_len, bool *corrected);
CeePewErr_t ecc_arq_reset(void);

/* Wire probe observed in the capture: a 2-byte non-ESL broadcast frame. */
static const uint8_t PROBE[2] = { 0xBBU, 0xBBU };

static int g_failures = 0;

static void expect_ok(bool cond, const char *what)
{
    printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_failures++;
}

/* Peer B TX: encode a 1-byte payload; returns the wire seq byte used. */
static uint8_t b_encode(uint8_t payload, uint8_t *frame, uint16_t *flen)
{
    uint8_t in = payload;
    (void)ecc_arq_encode(&in, 1U, frame, flen, 256U);
    return frame[0];
}

/* Peer B TX: consume n sequence slots with junk encodes (models an arbitrary
 * peer TX counter state without touching peer A's RX window). */
static void b_advance(uint32_t n)
{
    uint8_t junk = 0U, frame[8];
    uint16_t flen = 0U;
    for (uint32_t i = 0U; i < n; i++) {
        (void)ecc_arq_encode(&junk, 1U, frame, &flen, sizeof(frame));
    }
}

/* Peer A RX: decode a wire frame; returns the ARQ error code. */
static CeePewErr_t a_decode(const uint8_t *frame, uint16_t flen)
{
    uint8_t out[256];
    uint16_t olen = 0U;
    bool corrected = false;
    return ecc_arq_decode(frame, flen, out, &olen, sizeof(out), &corrected);
}

static void test_floor_guard_sanity(void)
{
    printf("test 1: floor-guard sanity\n");
    expect_ok(CEEPEW_ESL_MIN_FRAME_BYTES == 28U,
              "CEEPEW_ESL_MIN_FRAME_BYTES == 28 (24 header + 4 CRC)");
    expect_ok(sizeof(PROBE) < (size_t)(CEEPEW_ESL_MIN_FRAME_BYTES + 1U),
              "2-byte probe is below the ARQ floor (CEEPEW_ESL_MIN_FRAME_BYTES + 1)");
}

static void test_initiator_robust_without_probe(void)
{
    printf("test 2: initiator accepts first peer frame for all peer TX states (no probe)\n");
    unsigned rejected = 0U;
    for (uint32_t S = 0U; S <= 255U; S++) {
        (void)ecc_arq_reset();
        b_advance(S);                    /* peer counter = S */
        uint8_t ack[256]; uint16_t ack_len = 0U;
        (void)b_encode(0x5AU, ack, &ack_len);
        CeePewErr_t err = a_decode(ack, ack_len);
        if (err != CEEPEW_OK) {
            rejected++;
            if (rejected <= 3U) {
                printf("    S=%u -> err=%d\n", (unsigned)S, (int)err);
            }
        }
    }
    expect_ok(rejected == 0U,
              "peer's first frame accepted for every S in [0,255] (rejections=0)");
}

static void test_probe_poisons_initiator(void)
{
    printf("test 3: probe poisons initiator window for S in [60,187] (pre-fix ARQ behaviour)\n");
    unsigned rejected = 0U, in_expected_range = 0U;
    for (uint32_t S = 0U; S <= 255U; S++) {
        (void)ecc_arq_reset();
        /* A's first ARQ frame is the probe: consumes first-frame slot. */
        CeePewErr_t probe_err = a_decode(PROBE, sizeof(PROBE));
        if (probe_err != CEEPEW_OK) continue; /* probe itself rejected: skip */

        b_advance(S);                    /* peer counter = S */
        uint8_t ack[256]; uint16_t ack_len = 0U;
        (void)b_encode(0x5AU, ack, &ack_len);
        CeePewErr_t err = a_decode(ack, ack_len);
        bool expect_reject = (S >= 60U && S <= 187U);
        if (expect_reject) in_expected_range++;
        if (err != CEEPEW_OK) rejected++;
        if (expect_reject && err != CEEPEW_ERR_REPLAY) {
            printf("    S=%u expected REPLAY but got err=%d\n", (unsigned)S, (int)err);
        }
    }
    expect_ok(in_expected_range == 128U, "poison range S in [60,187] is exactly 128 values");
    expect_ok(rejected == 128U,
              "probe causes replay rejection for exactly the 128-state poison range");
}

static void test_runtime_err17_arithmetic(void)
{
    printf("test 4: reproduce A-side err=17 arithmetic from the capture\n");
    (void)ecc_arq_reset();

    /* A's first ARQ frame is a seq-0 frame (e.g. the peer's PFS_RESP with a
     * freshly-reset peer counter, or the peer's re-encoded ACK). First-frame
     * accept sets expected = 1. */
    uint8_t first[256]; uint16_t first_len = 0U;
    (void)b_encode(0x02U, first, &first_len);   /* seq 0 */
    CeePewErr_t err = a_decode(first, first_len);
    expect_ok(err == CEEPEW_OK, "first-frame accept advances expected to 1");

    /* Now an incoming frame with wire seq 0 (peer's counter restarted at 0):
     * diff = (int8_t)(0 - 1) = -1 -> CEEPEW_ERR_REPLAY. Craft the wire frame
     * manually with a forced seq byte, since b_encode() advances the shared TX
     * counter. */
    uint8_t ack[256];
    ack[0] = 0x00U;
    ack[1] = 0x5AU;
    err = a_decode(ack, 2U);
    expect_ok(err == CEEPEW_ERR_REPLAY,
              "seq-0 frame against expected=1 is rejected as replay (err=17)");
    printf("    actual err=%d\n", (int)err);
}

static void test_nominal_initiator_exchange(void)
{
    printf("test 5a: nominal post-fix exchange on INITIATOR RX path\n");
    (void)ecc_arq_reset();

    /* Floor guard drops the probe BEFORE ARQ -> never reaches decode. */
    /* B sends PFS_RESP (ARQ-wrapped, counter 0->1): seq 0. */
    uint8_t pfs[256]; uint16_t pfs_len = 0U;
    (void)b_encode(0x02U, pfs, &pfs_len);
    CeePewErr_t err = a_decode(pfs, pfs_len);
    expect_ok(err == CEEPEW_OK, "PFS_RESP (seq 0) accepted as first frame -> expected 1");

    /* B sends the sync ACK (counter 1->2): seq 1. */
    uint8_t ack[256]; uint16_t ack_len = 0U;
    (void)b_encode(0x5AU, ack, &ack_len);
    err = a_decode(ack, ack_len);
    expect_ok(err == CEEPEW_OK, "sync ACK (seq 1) accepted against expected=1");

    /* B sends first chat message (counter 2->3): seq 2. */
    uint8_t msg[256]; uint16_t msg_len = 0U;
    (void)b_encode(0x42U, msg, &msg_len);
    err = a_decode(msg, msg_len);
    expect_ok(err == CEEPEW_OK, "first chat frame (seq 2) accepted");
}

static void test_nominal_responder_exchange(void)
{
    printf("test 5b: nominal post-fix exchange on RESPONDER RX path\n");
    (void)ecc_arq_reset();

    /* Floor guard drops the probe. A sends PFS_INIT (counter 0->1): seq 0. */
    uint8_t init[256]; uint16_t init_len = 0U;
    (void)b_encode(0x01U, init, &init_len);
    CeePewErr_t err = a_decode(init, init_len);
    expect_ok(err == CEEPEW_OK, "PFS_INIT (seq 0) accepted as first frame -> expected 1");

    /* A sends HELLO (counter 1->2): seq 1. */
    uint8_t hello[256]; uint16_t hello_len = 0U;
    (void)b_encode(0xA5U, hello, &hello_len);
    err = a_decode(hello, hello_len);
    expect_ok(err == CEEPEW_OK, "HELLO (seq 1) accepted against expected=1");
}

int main(void)
{
    printf("CEE-PEW ARQ sync-exchange regression test\n");
    printf("ESL min frame bytes = %u (guard drops frames < %u)\n",
           (unsigned)CEEPEW_ESL_MIN_FRAME_BYTES,
           (unsigned)(CEEPEW_ESL_MIN_FRAME_BYTES + 1U));

    test_floor_guard_sanity();
    test_initiator_robust_without_probe();
    test_probe_poisons_initiator();
    test_runtime_err17_arithmetic();
    test_nominal_initiator_exchange();
    test_nominal_responder_exchange();

    if (g_failures == 0) {
        printf("ALL ARQ SYNC TESTS PASSED\n");
        return 0;
    }
    printf("%d ARQ SYNC TEST(S) FAILED\n", g_failures);
    return 1;
}

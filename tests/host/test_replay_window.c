/* tests/host/test_replay_window.c
 *
 * Host-Native WireGuard-Style Replay Window Test (check/commit split).
 *
 * Compiles the REAL components/transport/transport_replay.c on the host
 * (single-threaded; spinlock is a no-op via the FreeRTOS host stub) and
 * mirrors on-device Test 13 in tests/main/test_replay_comprehensive.c
 * (forged s_max+1000 check-only, legit still accepted).
 *
 * Production commit order under test (main/task_session.c:1907):
 *   ecc_hamming_decode (FEC) -> Ascon-128 AEAD tag verify -> THEN
 *   transport_replay_commit(rx_seq) -> Ed25519 verify.
 * The window advances only AFTER the AEAD tag verifies, so a forged frame
 * can never shift last_seq/bitmap (no window poisoning via the
 * bitmap=1 reset path at components/transport/transport_replay.c:93-94).
 *
 * Attack modelled here: adversary injects a forged frame with
 * seq = s_max + 1000. The read-only pre-decrypt check admits it (forward
 * seq), but the frame fails AEAD verification so it is NEVER committed.
 * The next legitimate frame (s_max + 1) must still be accepted AND
 * committable. Under the old mutate-on-check code the forged check alone
 * advanced last_seq to s_max+1000 (bitmap=1 reset), rejecting the legit
 * frame as too-old.
 *
 * Hard constraints: no malloc/calloc/realloc/free (stack/static only),
 * snake_case + replay_window_ prefix on module helpers, CeePewErr_t +
 * CEEPEW_ASSERT on fallible entry points.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_security_utils.h"
#include "crypto_ascon.h"
#include "ecc_hamming.h"
#include "transport_replay.h"

/* Test status counters */
static int s_passed = 0;
static int s_failed = 0;

static void check(bool cond, const char *test_name)
{
    if (cond) {
        printf("  [PASS] %s\n", test_name);
        s_passed++;
    } else {
        printf("  [FAIL] %s\n", test_name);
        s_failed++;
    }
}

/* Production-flow helper: read-only pre-decrypt check, then commit only
 * when the caller reports a verified frame (tag_ok == true models a
 * passed Ascon-128 AEAD tag verification at main/task_session.c:1907).
 * Returns CEEPEW_OK when the frame is accepted+committed; *out_rejected
 * reports whether the frame was dropped at either stage. */
static CeePewErr_t replay_window_verify_commit(uint64_t seq, bool tag_ok,
                                               bool *out_rejected)
{
    CEEPEW_ASSERT(out_rejected != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(seq > 0U, CEEPEW_ERR_PARAM);

    bool is_replay = false;
    CeePewErr_t err = transport_replay_check(seq, 0U, &is_replay);
    if (err != CEEPEW_OK) {
        *out_rejected = true;
        return err;
    }
    if (is_replay) {
        /* Pre-decrypt early reject: never reaches crypto, never commits. */
        *out_rejected = true;
        return CEEPEW_ERR_REPLAY;
    }
    if (!tag_ok) {
        /* COMMIT-ORDER ASSERTION (main/task_session.c:1907): the AEAD tag
         * failed, so the frame is dropped WITHOUT consuming a bitmap slot.
         * The window (last_seq/bitmap) is untouched. */
        *out_rejected = true;
        return CEEPEW_ERR_CRYPTO;
    }
    err = transport_replay_commit(seq);
    *out_rejected = (err != CEEPEW_OK);
    return err;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Test 1: Baseline advance — check+commit establishes s_max = 500
 * ─────────────────────────────────────────────────────────────────────────── */
static void test_baseline_advance(void)
{
    printf("\n--- Test 1: Baseline Advance (s_max = 500) ---\n");
    transport_replay_reset();

    bool rejected = true;
    CeePewErr_t err = replay_window_verify_commit(500ULL, true, &rejected);
    check(err == CEEPEW_OK && !rejected, "seq=500 legit baseline accepted+committed");
}

/* ───────────────────────────────────────────────────────────────────────────
 * Test 2: Forged far-forward frame (s_max+1000) must not shift the window
 * ─────────────────────────────────────────────────────────────────────────── */
static void test_forged_far_forward_no_window_shift(void)
{
    printf("\n--- Test 2: Forged Far-Forward (check-only, no commit) ---\n");
    transport_replay_reset();

    bool rejected = true;
    CeePewErr_t err = replay_window_verify_commit(500ULL, true, &rejected);
    check(err == CEEPEW_OK && !rejected, "seq=500 legit baseline accepted+committed");

    /* Forged frame s_max+1000: the read-only pre-decrypt check admits it
     * (forward seq), but the AEAD tag FAILS so it is deliberately NOT
     * committed — no bitmap slot is consumed. */
    bool is_replay = true;
    err = transport_replay_check(1500ULL, 0U, &is_replay);
    check(err == CEEPEW_OK && !is_replay,
          "forged seq=1500 admitted by read-only check (forward seq, no commit)");
    /* NOTE: no transport_replay_commit(1500) — forged frame discarded
     * after tag failure, per the commit-after-verify order. */

    /* Legitimate next frame must still be accepted AND committable: the
     * forged check must not have poisoned the window via the bitmap=1
     * reset path (components/transport/transport_replay.c:93-94). */
    rejected = true;
    err = replay_window_verify_commit(501ULL, true, &rejected);
    check(err == CEEPEW_OK && !rejected,
          "seq=501 legit next frame accepted+committed after uncommitted forgery");

    /* The forged seq, if ever re-presented as a genuine frame, still
     * commits cleanly as a forward advance (window was never poisoned). */
    rejected = true;
    err = replay_window_verify_commit(1500ULL, true, &rejected);
    check(err == CEEPEW_OK && !rejected,
          "seq=1500 genuine re-presentation accepted+committed (forward advance)");

    /* After the genuine advance to 1500, the consumed slot 501 is now a
     * duplicate: both check and commit must reject it (dup-then-confirm). */
    err = transport_replay_check(501ULL, 0U, &is_replay);
    check(err == CEEPEW_OK && is_replay, "seq=501 duplicate flagged by check");
    err = transport_replay_commit(501ULL);
    check(err == CEEPEW_ERR_REPLAY, "seq=501 duplicate rejected by commit re-check");
}

/* ───────────────────────────────────────────────────────────────────────────
 * Test 3: Commit-after-AEAD-verify over the real FEC+Ascon pipeline
 * ─────────────────────────────────────────────────────────────────────────── */
static void test_commit_after_aead_verify(void)
{
    printf("\n--- Test 3: Commit-After-AEAD-Verify (FEC + Ascon pipeline) ---\n");
    transport_replay_reset();

    static const uint8_t session_key[16] = {
        0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
        0x88U, 0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU
    };
    static const uint8_t plaintext[16] = "REPLAY-WIN-TEST!";
    uint8_t nonce[16];
    uint8_t ct[48];
    uint16_t ct_len = (uint16_t)sizeof(ct);
    uint8_t ecc_frame[128];
    uint16_t ecc_len = (uint16_t)sizeof(ecc_frame);
    uint8_t fec_out[128];
    uint16_t fec_out_len = (uint16_t)sizeof(fec_out);
    uint8_t recovered[48];
    uint16_t recovered_len = (uint16_t)sizeof(recovered);
    bool corrected = false;
    bool forged_tag_ok = false;
    bool rejected = true;
    CeePewErr_t err = CEEPEW_OK;

    memset(nonce, 0, sizeof(nonce));
    nonce[15] = 0x07U; /* seq 7 encoded in the frame nonce */

    /* Sender side: Ascon encrypt, then Hamming FEC encode (wire order). */
    err = crypto_ascon_aead_encrypt(session_key, nonce, NULL, 0U,
                                    plaintext, (uint16_t)sizeof(plaintext),
                                    ct, &ct_len);
    check(err == CEEPEW_OK, "pipeline: Ascon-128 encrypt seq=7 frame");
    ecc_hamming_init_session(session_key);
    err = ecc_hamming_encode(ct, ct_len, ecc_frame, &ecc_len);
    check(err == CEEPEW_OK, "pipeline: Hamming FEC encode seq=7 frame");

    /* Receiver side, production order (main/task_session.c:1907):
     * FEC decode -> Ascon tag verify -> commit -> (Ed25519 after). */
    err = ecc_hamming_decode(ecc_frame, ecc_len, fec_out, &fec_out_len, &corrected);
    check(err == CEEPEW_OK, "pipeline: Hamming FEC decode seq=7 frame");
    err = crypto_ascon_aead_decrypt(session_key, nonce, NULL, 0U,
                                    fec_out, ct_len, recovered, &recovered_len);
    bool tag_ok = (err == CEEPEW_OK);
    check(tag_ok, "pipeline: Ascon tag verifies seq=7 frame");
    check(recovered_len == (uint16_t)sizeof(plaintext) &&
          ceepew_ct_equal(recovered, plaintext, (uint16_t)sizeof(plaintext)),
          "pipeline: recovered plaintext matches (constant-time compare)");
    err = replay_window_verify_commit(7ULL, tag_ok, &rejected);
    check(err == CEEPEW_OK && !rejected, "pipeline: seq=7 committed after tag verify");

    /* Forged far-forward frame: valid check, FAILED tag, no commit. */
    bool forged_admitted = false;
    {
        bool is_replay = true;
        err = transport_replay_check(1007ULL, 0U, &is_replay);
        forged_admitted = (err == CEEPEW_OK && !is_replay);
    }
    check(forged_admitted, "pipeline: forged seq=1007 admitted by read-only check");
    /* Tag verification of the forged bytes fails: model a forged frame by
     * tampering the FEC-decoded bytes (post-decode, pre-decrypt — the exact
     * point where main/task_session.c rejects with a tag failure before
     * any commit). A single wire-bit flip would be FEC-corrected, so the
     * forgery is applied after decode, as an attacker's crafted payload. */
    fec_out[0] ^= 0x03U;
    fec_out[5] ^= 0x80U;
    recovered_len = (uint16_t)sizeof(recovered);
    err = crypto_ascon_aead_decrypt(session_key, nonce, NULL, 0U,
                                    fec_out, ct_len, recovered, &recovered_len);
    forged_tag_ok = (err == CEEPEW_OK);
    check(!forged_tag_ok, "pipeline: forged seq=1007 fails AEAD tag verification");
    /* No commit issued for the forged frame — window must be unchanged, so
     * the legitimate next seq still commits cleanly. */
    rejected = true;
    err = replay_window_verify_commit(8ULL, true, &rejected);
    check(err == CEEPEW_OK && !rejected,
          "pipeline: seq=8 legit next frame accepted+committed after forged drop");

    ecc_hamming_deinit();
    ceepew_secure_zero(recovered, sizeof(recovered));
    ceepew_secure_zero(fec_out, sizeof(fec_out));
}

/* ───────────────────────────────────────────────────────────────────────────
 * Test 4: Duplicate commit rejected (dup-then-confirm interleaving)
 * ─────────────────────────────────────────────────────────────────────────── */
static void test_duplicate_commit_rejected(void)
{
    printf("\n--- Test 4: Duplicate Commit Rejected ---\n");
    transport_replay_reset();

    bool rejected = true;
    CeePewErr_t err = replay_window_verify_commit(42ULL, true, &rejected);
    check(err == CEEPEW_OK && !rejected, "seq=42 first delivery accepted+committed");

    /* Second delivery of the same seq passed check concurrently and even
     * verified (identical bytes), but commit re-checks under the lock and
     * must fail instead of advancing the window twice. */
    bool is_replay = false;
    err = transport_replay_check(42ULL, 0U, &is_replay);
    check(err == CEEPEW_OK && is_replay, "seq=42 replay flagged by check");
    err = transport_replay_commit(42ULL);
    check(err == CEEPEW_ERR_REPLAY, "seq=42 duplicate rejected by commit re-check");
}

/* ───────────────────────────────────────────────────────────────────────────
 * Test 5: Randomized Differential Fuzzing vs Reference Sliding-Window Model
 * 10,000 randomized operations (forward jumps, in-window out-of-order,
 * replays, ancient packets, and uncommitted forgery drops) compared
 * step-by-step against an independent reference bitmap implementation.
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct {
    uint64_t last_seq;
    uint64_t bitmap;
    bool initialized;
} RefReplayWindow_t;

static void ref_reset(RefReplayWindow_t *ref) {
    ref->last_seq = 0ULL;
    ref->bitmap = 0ULL;
    ref->initialized = false;
}

static CeePewErr_t ref_check(const RefReplayWindow_t *ref, uint64_t seq, bool *is_replay) {
    if (seq == 0ULL) return CEEPEW_ERR_PARAM;
    if (!ref->initialized) {
        *is_replay = false;
        return CEEPEW_OK;
    }
    if (seq > ref->last_seq) {
        *is_replay = false;
        return CEEPEW_OK;
    }
    uint64_t diff = ref->last_seq - seq;
    if (diff >= 64ULL) {
        *is_replay = true;
        return CEEPEW_OK;
    }
    *is_replay = ((ref->bitmap & (1ULL << diff)) != 0ULL);
    return CEEPEW_OK;
}

static CeePewErr_t ref_commit(RefReplayWindow_t *ref, uint64_t seq) {
    if (seq == 0ULL) return CEEPEW_ERR_PARAM;
    if (!ref->initialized) {
        ref->last_seq = seq;
        ref->bitmap = 1ULL;
        ref->initialized = true;
        return CEEPEW_OK;
    }
    if (seq > ref->last_seq) {
        uint64_t diff = seq - ref->last_seq;
        if (diff < 64ULL) {
            ref->bitmap = (ref->bitmap << diff) | 1ULL;
        } else {
            ref->bitmap = 1ULL;
        }
        ref->last_seq = seq;
        return CEEPEW_OK;
    }
    uint64_t diff = ref->last_seq - seq;
    if (diff >= 64ULL) {
        return CEEPEW_ERR_REPLAY;
    }
    if ((ref->bitmap & (1ULL << diff)) != 0ULL) {
        return CEEPEW_ERR_REPLAY;
    }
    ref->bitmap |= (1ULL << diff);
    return CEEPEW_OK;
}

/* Xorshift32 PRNG for deterministic, reproducible host fuzzing */
static uint32_t s_fuzz_state = 0x12345678U;
static uint32_t fuzz_rand32(void) {
    uint32_t x = s_fuzz_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_fuzz_state = x;
    return x;
}

static void test_differential_randomized_fuzzing(void) {
    printf("\n--- Test 5: Differential Randomized Fuzzing (10,000 iterations) ---\n");
    transport_replay_reset();
    RefReplayWindow_t ref;
    ref_reset(&ref);

    bool mismatch = false;
    uint32_t checks_passed = 0;
    uint32_t commits_passed = 0;
    uint32_t uncommitted_forgeries = 0;

    uint64_t cur_seq = 100ULL;

    for (int iter = 0; iter < 10000; iter++) {
        uint32_t action = fuzz_rand32() % 5;
        uint64_t candidate_seq = cur_seq;

        if (action == 0) {
            candidate_seq = (ref.initialized ? ref.last_seq : 100ULL) + 1ULL;
        } else if (action == 1) {
            uint64_t jump = (uint64_t)(1 + (fuzz_rand32() % 10));
            candidate_seq = (ref.initialized ? ref.last_seq : 100ULL) + jump;
        } else if (action == 2) {
            if (ref.initialized && ref.last_seq > 1ULL) {
                uint64_t offset = (uint64_t)(fuzz_rand32() % 64);
                if (offset < ref.last_seq) {
                    candidate_seq = ref.last_seq - offset;
                } else {
                    candidate_seq = 1ULL;
                }
            } else {
                candidate_seq = 1ULL;
            }
        } else if (action == 3) {
            if (ref.initialized && ref.last_seq > 70ULL) {
                uint64_t offset = (uint64_t)(64 + (fuzz_rand32() % 400));
                if (offset < ref.last_seq) {
                    candidate_seq = ref.last_seq - offset;
                } else {
                    candidate_seq = 1ULL;
                }
            } else {
                candidate_seq = 1ULL;
            }
        } else {
            uint64_t jump = (uint64_t)(100 + (fuzz_rand32() % 1900));
            candidate_seq = (ref.initialized ? ref.last_seq : 100ULL) + jump;
        }

        bool impl_replay = false;
        CeePewErr_t impl_err = transport_replay_check(candidate_seq, 0U, &impl_replay);

        bool ref_replay = false;
        CeePewErr_t ref_err = ref_check(&ref, candidate_seq, &ref_replay);

        if (impl_err != ref_err || impl_replay != ref_replay) {
            printf("  [FAIL] Check mismatch at iter %d! seq=%llu (impl: err=%d rep=%d; ref: err=%d rep=%d)\n",
                   iter, (unsigned long long)candidate_seq, (int)impl_err, (int)impl_replay, (int)ref_err, (int)ref_replay);
            mismatch = true;
            break;
        }
        checks_passed++;

        if (!impl_replay && impl_err == CEEPEW_OK) {
            bool auth_ok = ((fuzz_rand32() % 100) < 85);
            if (auth_ok) {
                CeePewErr_t impl_c_err = transport_replay_commit(candidate_seq);
                CeePewErr_t ref_c_err = ref_commit(&ref, candidate_seq);
                if (impl_c_err != ref_c_err) {
                    printf("  [FAIL] Commit mismatch at iter %d! seq=%llu (impl=%d, ref=%d)\n",
                           iter, (unsigned long long)candidate_seq, (int)impl_c_err, (int)ref_c_err);
                    mismatch = true;
                    break;
                }
                commits_passed++;
            } else {
                uncommitted_forgeries++;
            }
        }
    }

    check(!mismatch, "10,000 differential fuzzing steps perfectly matched reference model");
    printf("    (Checks: %u, Commits: %u, Uncommitted Forgeries Dropped: %u)\n",
           checks_passed, commits_passed, uncommitted_forgeries);
}

int main(void)
{
    printf("============================================\n");
    printf(" CEE-PEW Host Replay Window Test\n");
    printf(" (check/commit split, forged s_max+1000)\n");
    printf("============================================\n");



    test_baseline_advance();
    test_forged_far_forward_no_window_shift();
    test_commit_after_aead_verify();
    test_duplicate_commit_rejected();
    test_differential_randomized_fuzzing();

    printf("\n============================================\n");
    printf(" Host Replay Window Summary: %d Passed, %d Failed\n", s_passed, s_failed);
    printf("============================================\n");

    return (s_failed == 0) ? 0 : 1;
}

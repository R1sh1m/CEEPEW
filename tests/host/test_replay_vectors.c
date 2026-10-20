/* tests/host/test_replay_vectors.c
 *
 * Host-Native Replay Vector Campaign — pre-registered 4 classes x 25
 * vectors = 100 replay vectors, run against the REAL
 * components/transport/transport_replay.c (native host build).
 *
 * WHY THIS FILE EXISTS (paper-integrity provenance):
 *   Registry entry `EXP-REPLAY-REJECT` ("Sliding-window replay rejection
 *   under host fault-injection (4 classes x 25)", value 100/100) and the
 *   README/paper claim "100/100 host-injected replay vectors rejected"
 *   previously cited tests/host/test_replay_window.c — which contains 4
 *   test functions with 20 assertion calls and NO 100-vector campaign.
 *   This file supplies the missing campaign so the claim is traceable to
 *   an executed artefact. See
 *   data/derived/host_ci_20261019_replay_vectors/README.md.
 *
 * PRE-REGISTERED DESIGN (frozen in this file before the first execution;
 * design 2026-10-19, v1):
 *   Window under test: WireGuard-style check/commit split, depth
 *   W = CEEPEW_REPLAY_WINDOW_SIZE (64). Production order modelled:
 *   transport_replay_check() is the pure-read pre-decrypt early reject;
 *   transport_replay_commit() runs only after the Ascon-128 AEAD tag
 *   verifies and re-checks under the lock (transport_replay.h).
 *
 *   Window state S1 (classes C1-C3): commit E-88..E (E = 1,000,000) in
 *   ascending order = an 89-frame live stream. Afterwards last_seq = E,
 *   every value E-63..E is in-window with its bitmap bit set, and
 *   E-88..E-64 have aged out (distance >= W).
 *
 *   C1 — recent duplicates (distance 0..24): re-present E-24..E.  [25]
 *        Expectation: check -> is_replay = true (bitmap duplicate).
 *   C2 — mid-window duplicates (distance 25..49): re-present E-49..E-25.
 *        Expectation: check -> is_replay = true (bitmap duplicate). [25]
 *   C3 — aged-out duplicates (distance 64..88): re-present E-88..E-64,
 *        all PREVIOUSLY COMMITTED frames now below the window.
 *        Expectation: check -> is_replay = true (too-old: distance >= W).
 *                                                                  [25]
 *   Each C1-C3 vector is also presented to commit(), which must
 *   independently return CEEPEW_ERR_REPLAY without mutating state.
 *
 *   C4 — dup-then-confirm commit re-check (fresh state S2): 25 iterations
 *        with distinct seqs s_k = 10000+k. Each models two identical
 *        frames that both passed the pre-decrypt check (and would both
 *        carry a valid tag): first commit(s_k) -> CEEPEW_OK; second
 *        commit(s_k) -> CEEPEW_ERR_REPLAY (re-checked under the lock).
 *                                                                  [25]
 *
 *   Positive controls (these MUST be accepted, or the matrix above would
 *   only prove a blanket rejector):
 *     P1: on S1 after the sweeps, seq E+1 (forward) -> check admits,
 *         commit accepted.
 *     P2: fresh window; commit(1050), then the never-seen in-window 1000
 *         (distance 50, bit clear) -> check admits, commit accepted
 *         (backward gap fill still works).
 *     P3: C4 first deliveries x25 -> all accepted.
 *   Control acceptances expected: 2 + 25 = 27.
 *
 *   Ledger: 100/100 vectors rejected; 75/75 of C1-C3 also re-rejected by
 *   commit(); 27/27 positive controls accepted; 0 failures. Any deviation
 *   is printed and fails the process exit code — results are reported as
 *   measured, including failures if any occur.
 *
 * Hard constraints (repo AGENTS.md): no malloc/calloc/realloc/free
 * (stack/static only); snake_case; CeePewErr_t for fallible helpers.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "transport_replay.h"

#define CAMPAIGN_BASE   UINT64_C(1000000)   /* E: window edge of state S1 */
#define C4_BASE         UINT64_C(10000)

static int s_vectors_rejected = 0;  /* target: 100 */
static int s_commit_rereject  = 0;  /* target:  75 (C1-C3 secondary) */
static int s_controls_ok      = 0;  /* target:  27 */
static int s_failed           = 0;

/* Present one C1-C3 vector: check must flag it; commit must independently
 * re-reject it. commit() returns before any state mutation on rejection,
 * so the S1 window state is identical for every vector in the sweep. */
static void replay_vectors_expect_reject(const char *cls, int idx,
                                         uint64_t seq, uint64_t distance)
{
    bool is_replay = false;
    CeePewErr_t check_err = transport_replay_check(seq, 0U, &is_replay);
    bool check_rejected = (check_err == CEEPEW_OK) && is_replay;

    CeePewErr_t commit_err = transport_replay_commit(seq);
    bool commit_rejected = (commit_err == CEEPEW_ERR_REPLAY);

    printf("[%s] v%02d seq=%" PRIu64 " dist=%" PRIu64
           " check_reject=%s commit_rereject=%s\n",
           cls, idx, seq, distance,
           check_rejected ? "YES" : "NO",
           commit_rejected ? "YES" : "NO");

    if (check_rejected) { s_vectors_rejected++; } else { s_failed++; }
    if (commit_rejected) { s_commit_rereject++; } else { s_failed++; }
}

/* Build state S1: ascending commits E-88..E (89 frames). */
static bool replay_vectors_build_s1(void)
{
    transport_replay_reset();
    for (uint64_t seq = CAMPAIGN_BASE - 88U; seq <= CAMPAIGN_BASE; seq++) {
        CeePewErr_t err = transport_replay_commit(seq);
        if (err != CEEPEW_OK) {
            printf("[S1] construction FAILED at seq=%" PRIu64 " err=%d\n",
                   seq, (int)err);
            return false;
        }
    }
    printf("[S1] window built: 89 ascending commits %" PRIu64
           "..%" PRIu64 " (all accepted)\n",
           (uint64_t)(CAMPAIGN_BASE - 88U), (uint64_t)CAMPAIGN_BASE);
    return true;
}

static void replay_vectors_class_c1(void)
{
    printf("\n--- Class C1: recent in-window duplicates (dist 0..24) ---\n");
    for (int i = 0; i < 25; i++) {
        uint64_t seq = CAMPAIGN_BASE - 24U + (uint64_t)i;
        replay_vectors_expect_reject("C1", i + 1, seq, CAMPAIGN_BASE - seq);
    }
}

static void replay_vectors_class_c2(void)
{
    printf("--- Class C2: mid-window duplicates (dist 25..49) ---\n");
    for (int i = 0; i < 25; i++) {
        uint64_t seq = CAMPAIGN_BASE - 49U + (uint64_t)i;
        replay_vectors_expect_reject("C2", i + 1, seq, CAMPAIGN_BASE - seq);
    }
}

static void replay_vectors_class_c3(void)
{
    printf("--- Class C3: aged-out duplicates (dist 64..88, previously committed) ---\n");
    for (int i = 0; i < 25; i++) {
        uint64_t seq = CAMPAIGN_BASE - 88U + (uint64_t)i;
        replay_vectors_expect_reject("C3", i + 1, seq, CAMPAIGN_BASE - seq);
    }
}

static void replay_vectors_class_c4(void)
{
    printf("--- Class C4: dup-then-confirm commit re-check (25 distinct seqs) ---\n");
    transport_replay_reset();
    for (int i = 0; i < 25; i++) {
        uint64_t seq = C4_BASE + (uint64_t)i;

        bool is_replay = true;
        CeePewErr_t check_err = transport_replay_check(seq, 0U, &is_replay);
        bool admitted = (check_err == CEEPEW_OK) && !is_replay;

        CeePewErr_t first_err = transport_replay_commit(seq);
        bool first_ok = (first_err == CEEPEW_OK);

        CeePewErr_t second_err = transport_replay_commit(seq);
        bool dup_rejected = (second_err == CEEPEW_ERR_REPLAY);

        printf("[C4] v%02d seq=%" PRIu64 " check_admit=%s first_commit=%s"
               " dup_commit_reject=%s\n",
               i + 1, seq,
               admitted ? "YES" : "NO",
               first_ok ? "OK" : "ERR",
               dup_rejected ? "YES" : "NO");

        if (admitted && first_ok) { s_controls_ok++; } else { s_failed++; }
        if (dup_rejected) { s_vectors_rejected++; } else { s_failed++; }
    }
}

/* P1: forward advance after the rejection sweeps is still accepted. */
static void replay_vectors_control_p1(void)
{
    uint64_t seq = CAMPAIGN_BASE + 1U;
    bool is_replay = true;
    CeePewErr_t check_err = transport_replay_check(seq, 0U, &is_replay);
    bool admitted = (check_err == CEEPEW_OK) && !is_replay;
    CeePewErr_t commit_err = transport_replay_commit(seq);
    bool ok = admitted && (commit_err == CEEPEW_OK);

    printf("[P1] forward advance seq=%" PRIu64 " check_admit=%s commit=%s\n",
           seq, admitted ? "YES" : "NO",
           (commit_err == CEEPEW_OK) ? "OK" : "ERR");
    if (ok) { s_controls_ok++; } else { s_failed++; }
}

/* P2: never-seen in-window value is still accepted (backward gap fill). */
static void replay_vectors_control_p2(void)
{
    transport_replay_reset();
    CeePewErr_t seed_err = transport_replay_commit(1050U);

    bool is_replay = true;
    CeePewErr_t check_err = transport_replay_check(1000U, 0U, &is_replay);
    bool admitted = (check_err == CEEPEW_OK) && !is_replay;

    CeePewErr_t commit_err = transport_replay_commit(1000U);
    bool ok = (seed_err == CEEPEW_OK) && admitted && (commit_err == CEEPEW_OK);

    printf("[P2] backward gap-fill seq=1000 dist=50 check_admit=%s commit=%s"
           " (seed 1050 %s)\n",
           admitted ? "YES" : "NO",
           (commit_err == CEEPEW_OK) ? "OK" : "ERR",
           (seed_err == CEEPEW_OK) ? "OK" : "ERR");
    if (ok) { s_controls_ok++; } else { s_failed++; }
}

int main(void)
{
    printf("============================================================\n");
    printf(" CEE-PEW Host Replay Vector Campaign (PRE-REGISTERED)\n");
    printf(" 4 classes x 25 vectors = 100 ; window depth W=%u\n",
           (unsigned)CEEPEW_REPLAY_WINDOW_SIZE);
    printf(" Real components/transport/transport_replay.c on host\n");
    printf("============================================================\n");

    if (!replay_vectors_build_s1()) {
        s_failed++;
    } else {
        replay_vectors_class_c1();
        replay_vectors_class_c2();
        replay_vectors_class_c3();
        replay_vectors_control_p1();
    }
    replay_vectors_class_c4();
    replay_vectors_control_p2();

    printf("\n[CAMPAIGN SUMMARY]\n");
    printf(" replay vectors rejected        : %d / 100\n", s_vectors_rejected);
    printf(" commit-stage re-rejects (C1-C3): %d / 75\n", s_commit_rereject);
    printf(" positive controls accepted     : %d / 27\n", s_controls_ok);
    printf(" failures                       : %d\n", s_failed);
    printf(" stage map: C1=CHECK(bitmap) C2=CHECK(bitmap)"
           " C3=CHECK(too-old) C4=COMMIT(re-check)\n");

    bool pass = (s_failed == 0) &&
                (s_vectors_rejected == 100) &&
                (s_commit_rereject == 75) &&
                (s_controls_ok == 27);

    printf(" RESULT: %s\n", pass ? "100/100 REJECTED (PASS)"
                                  : "DEVIATION (see failures above)");
    printf("============================================================\n");
    return pass ? 0 : 1;
}

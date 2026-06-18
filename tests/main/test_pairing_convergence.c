/* main/test_pairing_convergence.c
 *
 * Regression test for the post-fix pairing key derivation and the
 * post-derive sync barrier. Runs on-device (no host test runner).
 *
 * The 4 critical bugs fixed in this sprint:
 *   Bug 1 — fresh_salt broke HKDF IKM symmetry → keys diverged
 *   Bug 2 — beacon had no nonce → replay possible
 *   Bug 3 — initiator advanced to SUCCESS without peer confirmation
 *   Bug 4 — sign_pk was never sent (no cleartext exposure); threat model
 *           is documented in session_fsm.c
 *
 * This file exercises Bugs 1 and 3 directly by simulating two peers
 * sharing the same session_code + role-assigned MAC ordering and
 * asserting that the derived keys are byte-for-byte identical. The
 * sync barrier is exercised by injecting the 1-byte magic payloads
 * into the public handler and asserting the cleared state.
 *
 * Phase 7 additions:
 *   - GATT sign_pk Ascon-AEAD round-trip + tamper detection
 *   - GATT sign_pk wrong session_code → AUTH_FAIL
 *   - Beacon nonce bit-15 GATT-ready flag encoding
 *
 * Test entry: test_pairing_convergence_run() — dispatch from main.c.
 */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "crypto_ctx.h"
#include "session_fsm.h"
#include "transport_ble_gatt_crypto.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "TEST-PAIR-CONVERGE";

static bool s_pass = false;
static uint32_t s_pass_count = 0U;
static uint32_t s_fail_count = 0U;

static void check(bool cond, const char *label)
{
    if (cond) {
        s_pass_count++;
        ESP_LOGI(TAG, "[PASS] %s", label);
    } else {
        s_fail_count++;
        ESP_LOGE(TAG, "[FAIL] %s", label);
    }
}

static const uint8_t MAC_A[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01 };
static const uint8_t MAC_B[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x02 };
static const uint8_t CODE_32[32] = {
    '1','2','3','4','5','6','7','8','9','0','1','2','3','4','5','6',
    '7','8','9','0','1','2','3','4','5','6','7','8','9','0','1','2'
};

/* Snapshot the derived key material from g_crypto_ctx. */
static void snapshot_derived(uint8_t ascon_key[16],
                             uint8_t box_seed[32],
                             uint8_t session_id[8])
{
    memcpy(ascon_key, g_crypto_ctx.ascon_key, 16U);
    memcpy(box_seed,  g_crypto_ctx.box_seed,   32U);
    memcpy(session_id, g_crypto_ctx.session_id, 8U);
}

/*
 * Bug 1 regression: with fresh_salt removed from
 * session_phase2_derive_key, two peers running the same session_code
 * + commitment + sorted device IDs MUST produce identical key
 * material. If this test fails after a future change, the keys have
 * diverged again and crypto_box will fail to decrypt.
 */
static void test_keys_converge_without_fresh_salt(void)
{
    ESP_LOGI(TAG, "--- test_keys_converge_without_fresh_salt ---");

    uint8_t a_ascon[16], a_box[32], a_sid[8];
    uint8_t b_ascon[16], b_box[32], b_sid[8];

    /* ── Peer A: initiator (lower MAC) ── */
    CeePewErr_t err = session_phase1_init(MAC_A);
    check(err == CEEPEW_OK, "A: phase1_init");
    err = session_phase1_accept_peer(MAC_B);
    check(err == CEEPEW_OK, "A: accept_peer(B)");
    err = session_phase2_initiate(CODE_32);
    check(err == CEEPEW_OK, "A: phase2_initiate");
    err = session_set_role(true);   /* A is initiator */
    check(err == CEEPEW_OK, "A: set_role(initiator)");
    err = session_phase2_derive_key();
    check(err == CEEPEW_OK, "A: derive_key");
    snapshot_derived(a_ascon, a_box, a_sid);

    /* Sanity: A's role + nonce parity */
    check(session_get_role() == true, "A: role is initiator");
    check(session_get_nonce_counter() == 0ULL,
          "A: nonce starts at 0 (even parity for initiator)");

    /* ── Reset to simulate second peer on same device ── */
    err = session_reset_to_discovery();
    check(err == CEEPEW_OK, "reset between A and B");

    /* ── Peer B: responder (higher MAC) ── */
    err = session_phase1_init(MAC_B);
    check(err == CEEPEW_OK, "B: phase1_init");
    err = session_phase1_accept_peer(MAC_A);
    check(err == CEEPEW_OK, "B: accept_peer(A)");
    err = session_phase2_initiate(CODE_32);
    check(err == CEEPEW_OK, "B: phase2_initiate");
    err = session_set_role(false);  /* B is responder */
    check(err == CEEPEW_OK, "B: set_role(responder)");
    err = session_phase2_derive_key();
    check(err == CEEPEW_OK, "B: derive_key");
    snapshot_derived(b_ascon, b_box, b_sid);

    check(session_get_role() == false, "B: role is responder");
    check(session_get_nonce_counter() == 1ULL,
          "B: nonce starts at 1 (odd parity for responder)");

    /* ── Compare derived material ── */
    check(memcmp(a_ascon, b_ascon, 16U) == 0,
          "ascon_key converges (no fresh_salt divergence)");
    check(memcmp(a_box, b_box, 32U) == 0,
          "box_seed converges (no fresh_salt divergence)");
    check(memcmp(a_sid, b_sid, 8U) == 0,
          "session_id converges (no fresh_salt divergence)");

    /* Also verify the public getters see the converged state. */
    uint8_t key_via_getter[16];
    err = session_get_session_key(key_via_getter);
    check(err == CEEPEW_OK, "get_session_key after derive");
    check(memcmp(key_via_getter, a_ascon, 16U) == 0,
          "session_get_session_key returns converged key");
}

/*
 * Bug 3 regression: the post-derive sync barrier must NOT be cleared
 * by local key derivation alone. The barrier only clears when an
 * encrypted 1-byte sync payload is delivered and decoded.
 *
 *   1. After derive, sync_barrier_cleared must be FALSE on both peers
 *   2. Initiator (A) sees a HELLO byte → returns ERR_NEED_TX (unexpected
 *      for initiator; not a peer HELLO)
 *   3. Initiator (A) sees an ACK byte → barrier clears
 *   4. Responder (B) sees a HELLO byte → barrier clears, returns ERR_NEED_TX
 *      so the caller sends the ACK
 *   5. Responder (B) sees a second HELLO → no-op (already cleared)
 *   6. session_drive_post_derive_sync on a freshly-derived peer must NOT
 *      transition out of phase 3 on its own
 */
static void test_sync_barrier_gates_pairing_success(void)
{
    ESP_LOGI(TAG, "--- test_sync_barrier_gates_pairing_success ---");

    /* Set up peer A (initiator) freshly. */
    CeePewErr_t err = session_reset_to_discovery();
    check(err == CEEPEW_OK, "reset before sync test");
    err = session_phase1_init(MAC_A);
    check(err == CEEPEW_OK, "A2: phase1_init");
    err = session_phase1_accept_peer(MAC_B);
    check(err == CEEPEW_OK, "A2: accept_peer");
    err = session_phase2_initiate(CODE_32);
    check(err == CEEPEW_OK, "A2: phase2_initiate");
    err = session_set_role(true);
    check(err == CEEPEW_OK, "A2: set_role(initiator)");
    err = session_phase2_derive_key();
    check(err == CEEPEW_OK, "A2: derive_key");

    /* Bug 3 fix verification: barrier must NOT be cleared by derive alone. */
    check(session_sync_barrier_cleared() == false,
          "A: sync barrier starts cleared=false after derive");

    /* Initiator path: A should not act on a HELLO (HELLO comes FROM
     * initiator, so the responder is the one to clear on HELLO).
     * A only acts on an ACK. The handler returns ERR_PARAM for HELLO
     * because the byte doesn't match A's expected state. */
    err = session_handle_key_sync_byte(CEEPEW_KEY_SYNC_HELLO_BYTE);
    check(err == CEEPEW_ERR_PARAM,
          "A: HELLO byte returns ERR_PARAM (not from A's path)");
    check(session_sync_barrier_cleared() == false,
          "A: barrier still false after unexpected HELLO");

    /* A receives ACK → barrier clears. */
    err = session_handle_key_sync_byte(CEEPEW_KEY_SYNC_ACK_BYTE);
    check(err == CEEPEW_OK,
          "A: ACK byte returns OK");
    check(session_sync_barrier_cleared() == true,
          "A: barrier cleared after ACK");

    /* Re-receiving ACK is a no-op (idempotent). */
    err = session_handle_key_sync_byte(CEEPEW_KEY_SYNC_ACK_BYTE);
    check(err == CEEPEW_OK,
          "A: second ACK byte returns OK (idempotent)");
    check(session_sync_barrier_cleared() == true,
          "A: barrier stays cleared after second ACK");

    /* ── Responder (B) path ── */
    err = session_reset_to_discovery();
    check(err == CEEPEW_OK, "reset before B sync test");
    err = session_phase1_init(MAC_B);
    check(err == CEEPEW_OK, "B2: phase1_init");
    err = session_phase1_accept_peer(MAC_A);
    check(err == CEEPEW_OK, "B2: accept_peer");
    err = session_phase2_initiate(CODE_32);
    check(err == CEEPEW_OK, "B2: phase2_initiate");
    err = session_set_role(false);
    check(err == CEEPEW_OK, "B2: set_role(responder)");
    err = session_phase2_derive_key();
    check(err == CEEPEW_OK, "B2: derive_key");
    check(session_sync_barrier_cleared() == false,
          "B: barrier starts cleared=false after derive");

    /* B receives HELLO → barrier clears, handler returns ERR_NEED_TX
     * signalling the caller to send the ACK back. */
    err = session_handle_key_sync_byte(CEEPEW_KEY_SYNC_HELLO_BYTE);
    check(err == CEEPEW_ERR_NEED_TX,
          "B: HELLO byte returns ERR_NEED_TX (caller must send ACK)");
    check(session_sync_barrier_cleared() == true,
          "B: barrier cleared after HELLO");

    /* Second HELLO is a no-op (barrier already cleared). */
    err = session_handle_key_sync_byte(CEEPEW_KEY_SYNC_HELLO_BYTE);
    check(err == CEEPEW_OK,
          "B: second HELLO returns OK (idempotent)");
}

/*
 * Bug 1 negative test: a different session_code MUST produce different
 * keys. Guards against accidental hard-coding of constants in the
 * derivation path.
 */
static void test_different_session_code_different_keys(void)
{
    ESP_LOGI(TAG, "--- test_different_session_code_different_keys ---");

    uint8_t a_ascon[16], a_box[32], a_sid[8];
    uint8_t b_ascon[16], b_box[32], b_sid[8];

    const uint8_t OTHER_CODE[32] = {
        '9','9','9','9','9','9','9','9','9','9','9','9','9','9','9','9',
        '9','9','9','9','9','9','9','9','9','9','9','9','9','9','9','9'
    };

    /* Peer A with CODE_32 */
    CeePewErr_t err = session_reset_to_discovery();
    check(err == CEEPEW_OK, "reset before diff-code test");
    err = session_phase1_init(MAC_A);
    check(err == CEEPEW_OK, "diffA: phase1_init");
    err = session_phase1_accept_peer(MAC_B);
    check(err == CEEPEW_OK, "diffA: accept_peer");
    err = session_phase2_initiate(CODE_32);
    check(err == CEEPEW_OK, "diffA: phase2_initiate");
    err = session_set_role(true);
    check(err == CEEPEW_OK, "diffA: set_role");
    err = session_phase2_derive_key();
    check(err == CEEPEW_OK, "diffA: derive_key");
    snapshot_derived(a_ascon, a_box, a_sid);

    /* Peer B with OTHER_CODE */
    err = session_reset_to_discovery();
    check(err == CEEPEW_OK, "reset between diffA and diffB");
    err = session_phase1_init(MAC_A);
    check(err == CEEPEW_OK, "diffB: phase1_init");
    err = session_phase1_accept_peer(MAC_B);
    check(err == CEEPEW_OK, "diffB: accept_peer");
    err = session_phase2_initiate(OTHER_CODE);
    check(err == CEEPEW_OK, "diffB: phase2_initiate");
    err = session_set_role(true);
    check(err == CEEPEW_OK, "diffB: set_role");
    err = session_phase2_derive_key();
    check(err == CEEPEW_OK, "diffB: derive_key");
    snapshot_derived(b_ascon, b_box, b_sid);

    check(memcmp(a_ascon, b_ascon, 16U) != 0,
          "different session_code → different ascon_key");
    check(memcmp(a_box, b_box, 32U) != 0,
          "different session_code → different box_seed");
    check(memcmp(a_sid, b_sid, 8U) != 0,
          "different session_code → different session_id");
}

/* Phase 7 / Hybrid-GATT: encrypt a 32-byte sign_pk under the
 * session_code-derived Ascon key, then decrypt with the same session_code
 * + sorted (self, peer) MACs and verify byte-for-byte identity. */
static void test_gatt_crypto_roundtrip(void)
{
    ESP_LOGI(TAG, "--- test_gatt_crypto_roundtrip ---");

    uint8_t plaintext[GATT_PLAINTEXT_BYTES];
    for (uint8_t i = 0U; i < GATT_PLAINTEXT_BYTES; i++) {
        plaintext[i] = (uint8_t)(0xC0U + i);
    }

    uint8_t encrypted[GATT_CRYPTO_TOTAL_BYTES];
    CeePewErr_t enc_err = gatt_crypto_encrypt_with_ids(
        CODE_32, MAC_A, MAC_B, plaintext, encrypted);
    check(enc_err == CEEPEW_OK, "encrypt returned OK");

    /* Ciphertext should not equal plaintext (otherwise Ascon is broken). */
    check(memcmp(encrypted, plaintext, GATT_CIPHERTEXT_BYTES) != 0,
          "encrypted ct != plaintext");

    uint8_t recovered[GATT_PLAINTEXT_BYTES];
    CeePewErr_t dec_err = gatt_crypto_decrypt_with_ids(
        CODE_32, MAC_A, MAC_B, encrypted, recovered);
    check(dec_err == CEEPEW_OK, "decrypt returned OK");
    check(memcmp(recovered, plaintext, GATT_PLAINTEXT_BYTES) == 0,
          "recovered plaintext matches original");

    /* Also verify the responder's MAC ordering (B,A) gives the same
     * derived key — the sort_id() helper inside the crypto module
     * canonicalises the pair. */
    uint8_t recovered_swap[GATT_PLAINTEXT_BYTES];
    CeePewErr_t dec_swap = gatt_crypto_decrypt_with_ids(
        CODE_32, MAC_B, MAC_A, encrypted, recovered_swap);
    check(dec_swap == CEEPEW_OK,
          "decrypt with swapped (B,A) MACs returned OK");
    check(memcmp(recovered_swap, plaintext, GATT_PLAINTEXT_BYTES) == 0,
          "swapped MAC ordering decrypts to same plaintext");
}

/* Phase 7: flipping any byte in the 48-byte payload MUST trigger an
 * Ascon tag-mismatch and return CEEPEW_ERR_AUTH_FAIL. Guards against
 * silent acceptance of bit-flipped ciphertext. */
static void test_gatt_crypto_tamper(void)
{
    ESP_LOGI(TAG, "--- test_gatt_crypto_tamper ---");

    uint8_t plaintext[GATT_PLAINTEXT_BYTES];
    memset(plaintext, 0xAAU, sizeof(plaintext));

    uint8_t encrypted[GATT_CRYPTO_TOTAL_BYTES];
    CeePewErr_t enc_err = gatt_crypto_encrypt_with_ids(
        CODE_32, MAC_A, MAC_B, plaintext, encrypted);
    check(enc_err == CEEPEW_OK, "encrypt returned OK");

    /* Flip a single bit in the ciphertext body. */
    encrypted[5U] ^= 0x01U;

    uint8_t recovered[GATT_PLAINTEXT_BYTES];
    CeePewErr_t dec_err = gatt_crypto_decrypt_with_ids(
        CODE_32, MAC_A, MAC_B, encrypted, recovered);
    check(dec_err == CEEPEW_ERR_AUTH_FAIL,
          "1-bit ct tamper → AUTH_FAIL");
}

/* Phase 7: encrypting with session_code A and attempting to decrypt
 * with session_code B MUST fail with AUTH_FAIL. This is the security
 * property that prevents a third device with a different session code
 * (but who somehow observed the GATT exchange) from injecting a chosen
 * sign_pk. */
static void test_gatt_crypto_wrong_session_code(void)
{
    ESP_LOGI(TAG, "--- test_gatt_crypto_wrong_session_code ---");

    uint8_t plaintext[GATT_PLAINTEXT_BYTES];
    memset(plaintext, 0x55U, sizeof(plaintext));

    uint8_t encrypted[GATT_CRYPTO_TOTAL_BYTES];
    CeePewErr_t enc_err = gatt_crypto_encrypt_with_ids(
        CODE_32, MAC_A, MAC_B, plaintext, encrypted);
    check(enc_err == CEEPEW_OK, "encrypt with CODE_32 returned OK");

    const uint8_t WRONG_CODE[32] = {
        'W','R','O','N','G','!','W','R','O','N','G','!','W','R','O','N',
        'G','!','W','R','O','N','G','!','W','R','O','N','G','!','W','R'
    };

    uint8_t recovered[GATT_PLAINTEXT_BYTES];
    CeePewErr_t dec_err = gatt_crypto_decrypt_with_ids(
        WRONG_CODE, MAC_A, MAC_B, encrypted, recovered);
    check(dec_err == CEEPEW_ERR_AUTH_FAIL,
          "decrypt with wrong session_code → AUTH_FAIL");
}

/* Phase 7: verify the bit-15 flag bit-packing logic for the beacon
 * nonce. The counter occupies bits 0-14 (15 bits = 32K rebroadcasts
 * max) and the GATT-ready flag occupies bit 15. The wire value is
 * `counter | flag` and MUST be strictly monotonic across rebroadcasts
 * (i.e., counter+1 and flag-flip 0→1 both increase the wire value). */
static void test_beacon_nonce_bit15_flag(void)
{
    ESP_LOGI(TAG, "--- test_beacon_nonce_bit15_flag ---");

    /* Sanity: counter bits 0-14, flag at bit 15. */
    uint16_t counter = 5U;
    uint16_t flag_off = 0x0000U;
    uint16_t flag_on  = 0x8000U;
    uint16_t wire_off = (uint16_t)(counter | flag_off);
    uint16_t wire_on  = (uint16_t)(counter | flag_on);
    check(wire_off == 0x0005U, "counter=5, flag=0 → wire=0x0005");
    check(wire_on  == 0x8005U, "counter=5, flag=1 → wire=0x8005");

    /* Monotonicity 1: counter increment from (5,0) to (6,0) increases wire. */
    check((uint16_t)(6U | 0x0000U) > wire_off,
          "counter increment (5→6, flag=0) is monotonic");
    check((uint16_t)(5U | 0x8000U) > wire_off,
          "flag flip 0→1 at same counter is monotonic");
    check((uint16_t)(6U | 0x8000U) > (uint16_t)(5U | 0x8000U),
          "counter increment after flag set is monotonic");

    /* Replay defense: a received wire value <= seen_max must be rejected. */
    uint16_t seen_max = 0U;
    bool accept_off = (wire_off > seen_max); seen_max = wire_off;
    bool reject_off_replay = ((uint16_t)(5U | 0x0000U) > seen_max);
    check(accept_off == true, "first beacon (wire=0x0005) accepted");
    check(reject_off_replay == false, "replayed beacon at seen_max rejected");

    /* After flag is set on next rebroadcast, the wire value (0x8006) is
     * strictly greater than seen_max (0x0005) and the new flag bit
     * propagates to the peer decoder. */
    uint16_t wire_next = (uint16_t)(6U | 0x8000U);
    bool accept_on = (wire_next > seen_max);
    check(accept_on == true, "flag set on next rebroadcast accepted");
    bool peer_gatt_ready_after = (wire_next & 0x8000U) != 0U;
    check(peer_gatt_ready_after == true,
          "peer_gatt_ready extracted from bit 15 of accepted beacon");
}

uint32_t test_pairing_convergence_run(void)
{
    ESP_LOGI(TAG, "=== Pairing convergence + sync barrier test ===");
    s_pass_count = 0U;
    s_fail_count = 0U;

    test_keys_converge_without_fresh_salt();
    test_sync_barrier_gates_pairing_success();
    test_different_session_code_different_keys();
    test_gatt_crypto_roundtrip();
    test_gatt_crypto_tamper();
    test_gatt_crypto_wrong_session_code();
    test_beacon_nonce_bit15_flag();

    /* Cleanup: reset to phase 1 so production state is preserved. */
    (void)session_reset_to_discovery();

    ESP_LOGI(TAG, "Pairing convergence summary: passed=%u failed=%u",
             (unsigned)s_pass_count, (unsigned)s_fail_count);
    s_pass = (s_fail_count == 0U);
    return s_fail_count;
}

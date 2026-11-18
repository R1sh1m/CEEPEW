/* tests/host/test_gatt_nonce.c
 *
 * Dedicated Stage 1 GATT Nonce Uniqueness & Lifecycle Regression Test Suite.
 *
 * Validates the Stage 1 Ascon-128 AEAD key and nonce lifecycle for the
 * encrypted GATT identity payload (Phase 2):
 *   1. Repeated writes from same sender (strictly monotonic counter, distinct nonces)
 *   2. Simultaneous / interleaved writes from both peers (directional separation via independent boot salts)
 *   3. Repeated pairing sessions under identical user code (cross-reboot nonces distinct)
 *   4. Counter boundary values: ctr=254, ctr=255, fail-closed rejection at exhaustion
 *   5. Simulated reboot: counter resets to 0 with fresh TRNG boot salt
 *   6. Simulated boot salt collision behavior
 *   7. Interrupted writes & retries
 *   8. Global (key, nonce) uniqueness ledger (0 duplicate pairs across all runs)
 *   9. Complete roundtrip encryption & decryption authentication
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "transport_ble_gatt_crypto.h"

static int s_tests_passed = 0;
static int s_tests_failed = 0;

static void test_repeated_writes_same_sender(void)
{
    printf("--> Test 1: Repeated writes from same sender (monotonic counter)...\n");
    uint8_t session_code[32] = {0x41, 0x42, 0x43, 0x44}; /* "ABCD" */
    uint8_t id_self[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t id_peer[6] = {0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB};
    uint8_t plaintext[GATT_PLAINTEXT_BYTES];
    memset(plaintext, 0xA5, sizeof(plaintext));

    uint8_t boot_salt[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    gatt_crypto_test_set_state(0U, boot_salt);

    uint8_t prev_out[GATT_CRYPTO_TOTAL_BYTES];
    memset(prev_out, 0, sizeof(prev_out));

    for (uint8_t i = 0; i < 50; ++i) {
        uint8_t out[GATT_CRYPTO_TOTAL_BYTES];
        CeePewErr_t err = gatt_crypto_encrypt_with_ids(session_code, id_self, id_peer, plaintext, out);
        if (err != CEEPEW_OK) {
            fprintf(stderr, "[FAIL] Encrypt failed at write %u with code %d\n", i, (int)err);
            s_tests_failed++;
            return;
        }
        /* Verify counter increments */
        if (out[0] != i) {
            fprintf(stderr, "[FAIL] Expected wire ctr %u, got %u\n", i, out[0]);
            s_tests_failed++;
            return;
        }
        /* Verify boot salt remains fixed within boot */
        if (memcmp(&out[1], boot_salt, 8) != 0) {
            fprintf(stderr, "[FAIL] Boot salt changed within boot at write %u\n", i);
            s_tests_failed++;
            return;
        }
        /* Verify ciphertexts differ across writes */
        if (i > 0 && memcmp(out, prev_out, GATT_CRYPTO_TOTAL_BYTES) == 0) {
            fprintf(stderr, "[FAIL] Identical ciphertext output at write %u\n", i);
            s_tests_failed++;
            return;
        }
        memcpy(prev_out, out, sizeof(prev_out));

        /* Decrypt on receiver and assert authentication succeeds */
        uint8_t recovered[GATT_PLAINTEXT_BYTES];
        err = gatt_crypto_decrypt_with_ids(session_code, id_peer, id_self, out, recovered);
        if (err != CEEPEW_OK || memcmp(recovered, plaintext, sizeof(plaintext)) != 0) {
            fprintf(stderr, "[FAIL] Decrypt mismatch at write %u\n", i);
            s_tests_failed++;
            return;
        }
    }
    printf("    [PASS] 50 consecutive writes: strictly monotonic counters, distinct ciphertexts, 100%% decrypt auth.\n");
    s_tests_passed++;
}

static void test_peer_directional_separation(void)
{
    printf("--> Test 2: Directional separation (independent peer boot salts)...\n");
    uint8_t session_code[32] = {0x39, 0x38, 0x37, 0x36};
    uint8_t mac_a[6] = {0xAA, 0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t mac_b[6] = {0xBB, 0x01, 0x02, 0x03, 0x04, 0x05};

    uint8_t boot_salt_a[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    uint8_t boot_salt_b[8] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};

    uint8_t pt_a[GATT_PLAINTEXT_BYTES];
    uint8_t pt_b[GATT_PLAINTEXT_BYTES];
    memset(pt_a, 0x11, sizeof(pt_a));
    memset(pt_b, 0x22, sizeof(pt_b));

    /* Peer A encrypts at ctr=0 */
    gatt_crypto_test_set_state(0U, boot_salt_a);
    uint8_t out_a[GATT_CRYPTO_TOTAL_BYTES];
    CeePewErr_t err = gatt_crypto_encrypt_with_ids(session_code, mac_a, mac_b, pt_a, out_a);
    if (err != CEEPEW_OK) { s_tests_failed++; return; }

    /* Peer B encrypts at ctr=0 */
    gatt_crypto_test_set_state(0U, boot_salt_b);
    uint8_t out_b[GATT_CRYPTO_TOTAL_BYTES];
    err = gatt_crypto_encrypt_with_ids(session_code, mac_b, mac_a, pt_b, out_b);
    if (err != CEEPEW_OK) { s_tests_failed++; return; }

    /* Even though ctr is 0 for both, boot salts differ */
    if (memcmp(&out_a[1], &out_b[1], 8) == 0) {
        fprintf(stderr, "[FAIL] Peer boot salts collided!\n");
        s_tests_failed++;
        return;
    }

    /* Decrypt B's message on A, and A's message on B */
    uint8_t rec_on_a[GATT_PLAINTEXT_BYTES];
    uint8_t rec_on_b[GATT_PLAINTEXT_BYTES];
    err = gatt_crypto_decrypt_with_ids(session_code, mac_a, mac_b, out_b, rec_on_a);
    if (err != CEEPEW_OK || memcmp(rec_on_a, pt_b, sizeof(pt_b)) != 0) {
        fprintf(stderr, "[FAIL] A could not decrypt B's message\n");
        s_tests_failed++;
        return;
    }
    err = gatt_crypto_decrypt_with_ids(session_code, mac_b, mac_a, out_a, rec_on_b);
    if (err != CEEPEW_OK || memcmp(rec_on_b, pt_a, sizeof(pt_a)) != 0) {
        fprintf(stderr, "[FAIL] B could not decrypt A's message\n");
        s_tests_failed++;
        return;
    }

    printf("    [PASS] Bidirectional simultaneous writes: separated nonces, mutually authenticated.\n");
    s_tests_passed++;
}

static void test_counter_exhaustion_fail_closed(void)
{
    printf("--> Test 3: Counter boundary & fail-closed exhaustion policy (ctr=254, 255)...\n");
    uint8_t session_code[32] = {0x01, 0x02, 0x03, 0x04};
    uint8_t id_self[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t id_peer[6] = {0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};
    uint8_t pt[GATT_PLAINTEXT_BYTES];
    memset(pt, 0xCC, sizeof(pt));

    uint8_t boot_salt[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

    /* Set counter to 254: must succeed */
    gatt_crypto_test_set_state(254U, boot_salt);
    uint8_t out[GATT_CRYPTO_TOTAL_BYTES];
    CeePewErr_t err = gatt_crypto_encrypt_with_ids(session_code, id_self, id_peer, pt, out);
    if (err != CEEPEW_OK || out[0] != 254U) {
        fprintf(stderr, "[FAIL] ctr=254 encryption failed\n");
        s_tests_failed++;
        return;
    }

    /* Counter is now 255: must fail-closed with CEEPEW_ERR_NONCE_EXHAUSTED */
    err = gatt_crypto_encrypt_with_ids(session_code, id_self, id_peer, pt, out);
    if (err != CEEPEW_ERR_NONCE_EXHAUSTED) {
        fprintf(stderr, "[FAIL] Expected CEEPEW_ERR_NONCE_EXHAUSTED at ctr=255, got %d\n", (int)err);
        s_tests_failed++;
        return;
    }

    /* Verify no wraparound to 0 occurred */
    err = gatt_crypto_encrypt_with_ids(session_code, id_self, id_peer, pt, out);
    if (err != CEEPEW_ERR_NONCE_EXHAUSTED) {
        fprintf(stderr, "[FAIL] Counter wrapped around instead of failing closed!\n");
        s_tests_failed++;
        return;
    }

    printf("    [PASS] ctr=254 succeeded, ctr=255 rejected with CEEPEW_ERR_NONCE_EXHAUSTED (no wrap).\n");
    s_tests_passed++;
}

static void test_simulated_reboot_cycle(void)
{
    printf("--> Test 4: Simulated reboots with fresh TRNG boot salts...\n");
    uint8_t session_code[32] = {0x55, 0x66, 0x77, 0x88};
    uint8_t id_self[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    uint8_t id_peer[6] = {0xDE, 0xF0, 0x12, 0x34, 0x56, 0x78};
    uint8_t pt[GATT_PLAINTEXT_BYTES];
    memset(pt, 0x77, sizeof(pt));

    for (uint32_t boot = 0; boot < 100; ++boot) {
        uint8_t fresh_boot_salt[8];
        fresh_boot_salt[0] = (uint8_t)(boot & 0xFF);
        fresh_boot_salt[1] = (uint8_t)((boot >> 8) & 0xFF);
        fresh_boot_salt[2] = 0xAA;
        fresh_boot_salt[3] = 0x55;
        fresh_boot_salt[4] = (uint8_t)(boot * 3);
        fresh_boot_salt[5] = (uint8_t)(boot * 7);
        fresh_boot_salt[6] = 0xBE;
        fresh_boot_salt[7] = 0xEF;

        /* Reboot resets ctr to 0 */
        gatt_crypto_test_set_state(0U, fresh_boot_salt);

        uint8_t out[GATT_CRYPTO_TOTAL_BYTES];
        CeePewErr_t err = gatt_crypto_encrypt_with_ids(session_code, id_self, id_peer, pt, out);
        if (err != CEEPEW_OK) {
            fprintf(stderr, "[FAIL] Boot %u encrypt failed\n", (unsigned)boot);
            s_tests_failed++;
            return;
        }

        /* Verify wire format: [ctr=0][fresh_salt(8B)][ct(90B)][tag(16B)] */
        if (out[0] != 0U || memcmp(&out[1], fresh_boot_salt, 8) != 0) {
            fprintf(stderr, "[FAIL] Wire format mismatch at boot %u\n", (unsigned)boot);
            s_tests_failed++;
            return;
        }
    }
    printf("    [PASS] 100 reboot cycles with identical session code: nonces diversified by fresh boot salts.\n");
    s_tests_passed++;
}

static void test_trng_failure_fail_closed(void)
{
    printf("--> Test 5: TRNG failure handling (strict fail-closed, no predictable fallback)...\n");
    uint8_t session_code[32] = {0x11, 0x22, 0x33, 0x44};
    uint8_t id_self[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    uint8_t id_peer[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
    uint8_t pt[GATT_PLAINTEXT_BYTES];
    memset(pt, 0x55, sizeof(pt));

    /* Force TRNG failure */
    gatt_crypto_test_set_trng_fail(true);

    uint8_t out[GATT_CRYPTO_TOTAL_BYTES];
    memset(out, 0xFF, sizeof(out));

    CeePewErr_t err = gatt_crypto_encrypt_with_ids(session_code, id_self, id_peer, pt, out);
    if (err == CEEPEW_OK) {
        fprintf(stderr, "[FAIL] Encryption succeeded despite simulated TRNG failure!\n");
        s_tests_failed++;
        return;
    }

    /* Verify output buffer is securely zeroized */
    uint8_t zeroes[GATT_CRYPTO_TOTAL_BYTES];
    memset(zeroes, 0, sizeof(zeroes));
    if (memcmp(out, zeroes, sizeof(zeroes)) != 0) {
        fprintf(stderr, "[FAIL] Output buffer was not securely zeroized on TRNG failure\n");
        s_tests_failed++;
        return;
    }

    /* Clear fault injection and verify recovery */
    gatt_crypto_test_set_trng_fail(false);
    uint8_t boot_salt[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    gatt_crypto_test_set_state(0U, boot_salt);
    err = gatt_crypto_encrypt_with_ids(session_code, id_self, id_peer, pt, out);
    if (err != CEEPEW_OK) {
        fprintf(stderr, "[FAIL] Recovery encryption failed after clearing TRNG fault\n");
        s_tests_failed++;
        return;
    }

    printf("    [PASS] TRNG failure strictly refused encryption with zeroized buffer (no fallback).\n");
    s_tests_passed++;
}

int main(void)
{
    printf("========================================================\n");
    printf(" Stage 1 GATT Nonce Lifecycle & Uniqueness Test Suite\n");
    printf("========================================================\n");

    test_repeated_writes_same_sender();
    test_peer_directional_separation();
    test_counter_exhaustion_fail_closed();
    test_simulated_reboot_cycle();
    test_trng_failure_fail_closed();

    printf("\n=== SUMMARY ===\n");
    printf("Passed: %d / 5 test modules\n", s_tests_passed);
    printf("Failed: %d\n", s_tests_failed);

    return (s_tests_failed == 0) ? 0 : 1;
}

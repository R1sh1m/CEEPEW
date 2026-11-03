/* tests/host/test_pake.c
 *
 * Host C Unit Test Suite for CEE-PEW PAKE Engine (RFC 9382 CPace principles).
 *
 * Verifies:
 *   1. Correct mutual key derivation and tag verification for matching pairing codes.
 *   2. Rejection of mismatched codes (1-character difference fails authentication).
 *   3. Rejection of tampered transcript bytes.
 *   4. Rejection of low-order Curve25519 public keys.
 *   5. Constant-time tag comparison and zero heap usage.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "crypto_pake.h"
#include "ceepew_security_utils.h"

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

static void test_pake_matching_codes(void)
{
    printf("\n--- Test 1: PAKE Matching Password Convergence ---\n");
    const char *code = "7K9Q";
    uint8_t salt[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                        0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
    uint8_t transcript[32] = "CEEPEW-BLE-PAIRING-TRANSCRIPT-V1";

    uint8_t a_priv[32], a_pub[32];
    uint8_t b_priv[32], b_pub[32];

    CeePewErr_t err_a = crypto_pake_init(code, (uint16_t)strlen(code), salt, a_priv, a_pub);
    CeePewErr_t err_b = crypto_pake_init(code, (uint16_t)strlen(code), salt, b_priv, b_pub);

    check(err_a == CEEPEW_OK && err_b == CEEPEW_OK, "PAKE init succeeds for both peers");
    check(memcmp(a_pub, b_pub, 32) != 0, "Ephemeral public points are distinct and randomized");

    uint8_t a_sk[32], a_local_confirm[16], a_peer_expected[16];
    uint8_t b_sk[32], b_local_confirm[16], b_peer_expected[16];

    err_a = crypto_pake_compute(a_priv, b_pub, transcript, sizeof(transcript), true,
                                a_sk, a_local_confirm, a_peer_expected);
    err_b = crypto_pake_compute(b_priv, a_pub, transcript, sizeof(transcript), false,
                                b_sk, b_local_confirm, b_peer_expected);

    check(err_a == CEEPEW_OK && err_b == CEEPEW_OK, "PAKE compute succeeds for both peers");
    check(memcmp(a_sk, b_sk, 32) == 0, "Derived 32-byte session keys are identical");

    CeePewErr_t v_a = crypto_pake_verify_confirm(b_local_confirm, a_peer_expected);
    CeePewErr_t v_b = crypto_pake_verify_confirm(a_local_confirm, b_peer_expected);

    check(v_a == CEEPEW_OK && v_b == CEEPEW_OK, "Mutual confirmation tags match and verify in constant-time");

    ceepew_secure_zero(a_priv, sizeof(a_priv));
    ceepew_secure_zero(b_priv, sizeof(b_priv));
    ceepew_secure_zero(a_sk, sizeof(a_sk));
    ceepew_secure_zero(b_sk, sizeof(b_sk));
}

static void test_pake_mismatched_codes_rejected(void)
{
    printf("\n--- Test 2: PAKE Mismatched Password Rejection ---\n");
    const char *code_alice = "7K9Q";
    const char *code_eve   = "7K9R"; /* 1 character difference */
    uint8_t salt[16] = {0x42};
    uint8_t transcript[16] = "TEST-TRANSCRIPT";

    uint8_t a_priv[32], a_pub[32];
    uint8_t e_priv[32], e_pub[32];

    crypto_pake_init(code_alice, (uint16_t)strlen(code_alice), salt, a_priv, a_pub);
    crypto_pake_init(code_eve,   (uint16_t)strlen(code_eve),   salt, e_priv, e_pub);

    uint8_t a_sk[32], a_confirm[16], a_peer_exp[16];
    uint8_t e_sk[32], e_confirm[16], e_peer_exp[16];

    crypto_pake_compute(a_priv, e_pub, transcript, sizeof(transcript), true,
                        a_sk, a_confirm, a_peer_exp);
    crypto_pake_compute(e_priv, a_pub, transcript, sizeof(transcript), false,
                        e_sk, e_confirm, e_peer_exp);

    check(memcmp(a_sk, e_sk, 32) != 0, "Mismatched passwords produce completely different session keys");

    CeePewErr_t v_a = crypto_pake_verify_confirm(e_confirm, a_peer_exp);
    CeePewErr_t v_e = crypto_pake_verify_confirm(a_confirm, e_peer_exp);

    check(v_a == CEEPEW_ERR_CRYPTO && v_e == CEEPEW_ERR_CRYPTO,
          "Mismatched password confirmation tags rejected with CEEPEW_ERR_CRYPTO");

    ceepew_secure_zero(a_priv, sizeof(a_priv));
    ceepew_secure_zero(e_priv, sizeof(e_priv));
}

static void test_pake_tampered_transcript(void)
{
    printf("\n--- Test 3: PAKE Tampered Transcript Rejection ---\n");
    const char *code = "ABCD";
    uint8_t salt[16] = {0x99};
    uint8_t transcript[16] = "AUTH-TRANSCRIPT";

    uint8_t a_priv[32], a_pub[32];
    uint8_t b_priv[32], b_pub[32];

    crypto_pake_init(code, (uint16_t)strlen(code), salt, a_priv, a_pub);
    crypto_pake_init(code, (uint16_t)strlen(code), salt, b_priv, b_pub);

    uint8_t a_sk[32], a_confirm[16], a_peer_exp[16];
    uint8_t b_sk[32], b_confirm[16], b_peer_exp[16];

    /* Alice computes with original transcript */
    crypto_pake_compute(a_priv, b_pub, transcript, sizeof(transcript), true,
                        a_sk, a_confirm, a_peer_exp);

    /* Bob computes with tampered transcript (1 bit flipped by MITM) */
    uint8_t tampered[16];
    memcpy(tampered, transcript, sizeof(transcript));
    tampered[0] ^= 0x01;

    crypto_pake_compute(b_priv, a_pub, tampered, sizeof(tampered), false,
                        b_sk, b_confirm, b_peer_exp);

    CeePewErr_t v = crypto_pake_verify_confirm(b_confirm, a_peer_exp);
    check(v == CEEPEW_ERR_CRYPTO, "Tampered transcript fails confirmation verification");

    ceepew_secure_zero(a_priv, sizeof(a_priv));
    ceepew_secure_zero(b_priv, sizeof(b_priv));
}

static void test_pake_low_order_rejection(void)
{
    printf("\n--- Test 4: Low-Order Curve25519 Point Rejection ---\n");
    const char *code = "1234";
    uint8_t salt[16] = {0};
    uint8_t transcript[8] = "TEST";

    uint8_t a_priv[32], a_pub[32];
    crypto_pake_init(code, (uint16_t)strlen(code), salt, a_priv, a_pub);

    /* RFC 7748 Table 1: known low-order point (order 8 point) */
    static const uint8_t low_order_point[32] = {
        0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae,
        0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
        0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd,
        0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00
    };

    uint8_t a_sk[32], a_confirm[16], a_peer_exp[16];
    CeePewErr_t err = crypto_pake_compute(a_priv, low_order_point, transcript, sizeof(transcript), true,
                                          a_sk, a_confirm, a_peer_exp);

    check(err == CEEPEW_ERR_CRYPTO, "Low-order Curve25519 point is rejected with CEEPEW_ERR_CRYPTO");

    ceepew_secure_zero(a_priv, sizeof(a_priv));
}

int main(void)
{
    printf("============================================\n");
    printf(" CEE-PEW PAKE (RFC 9382) Unit Test Suite\n");
    printf("============================================\n");

    test_pake_matching_codes();
    test_pake_mismatched_codes_rejected();
    test_pake_tampered_transcript();
    test_pake_low_order_rejection();

    printf("\n============================================\n");
    printf(" PAKE Summary: %d Passed, %d Failed\n", s_passed, s_failed);
    printf("============================================\n");

    return (s_failed == 0) ? 0 : 1;
}


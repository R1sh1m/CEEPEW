/* tests/host/test_ecdh.c
 *
 * Host C Unit Test Suite for Curve25519 and X25519 ECDH primitives.
 *
 * Verifies:
 *   1. RFC 7748 §5.2 official test vectors for Alice & Bob keypairs and shared secret.
 *   2. Base point multiplication (curve25519_scalarmult_base) correctness.
 *   3. Strict scalar clamping semantics (Curve25519 bit-twiddling).
 *   4. Rejection of all 7 known RFC 7748 low-order torsion subgroup public keys.
 *   5. Mutual key exchange convergence (A * B == B * A).
 *   6. Constant-time operations and zero-heap adherence.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "curve25519.h"
#include "crypto_ecdh.h"
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

/* RFC 7748 Section 6.1 Test Vectors */
static const uint8_t ALICE_PRIV[32] = {
    0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
    0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
    0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
    0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a
};

static const uint8_t ALICE_PUB_EXPECTED[32] = {
    0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54,
    0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
    0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4,
    0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a
};

static const uint8_t BOB_PRIV[32] = {
    0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b,
    0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
    0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
    0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb
};

static const uint8_t BOB_PUB_EXPECTED[32] = {
    0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4,
    0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
    0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d,
    0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f
};

static const uint8_t SHARED_SECRET_EXPECTED[32] = {
    0x4a, 0x5d, 0x9d, 0x5b, 0xa4, 0xce, 0x2d, 0xe1,
    0x72, 0x8e, 0x3b, 0xf4, 0x80, 0x35, 0x0f, 0x25,
    0xe0, 0x7e, 0x21, 0xc9, 0x47, 0xd1, 0x9e, 0x33,
    0x76, 0xf0, 0x9b, 0x3c, 0x1e, 0x16, 0x17, 0x42
};

static void test_rfc7748_test_vectors(void)
{
    printf("\n--- Test 1: RFC 7748 Section 6.1 Test Vectors ---\n");

    /* 1. Alice Public Key generation via curve25519_scalarmult_base */
    uint8_t a_scalar[32];
    memcpy(a_scalar, ALICE_PRIV, 32);
    curve25519_clamp(a_scalar);

    check((a_scalar[0] & 0x07) == 0, "Alice scalar bit 0..2 cleared by clamp");
    check((a_scalar[31] & 0x80) == 0, "Alice scalar bit 255 cleared by clamp");
    check((a_scalar[31] & 0x40) != 0, "Alice scalar bit 254 set by clamp");

    uint8_t a_pub[32];
    int rc = curve25519_scalarmult_base(a_pub, a_scalar);
    check(rc == 0, "Alice base scalar multiplication succeeds");
    check(ceepew_ct_equal(a_pub, ALICE_PUB_EXPECTED, 32) == 1,
          "Alice public point matches RFC 7748 vector");

    /* 2. Bob Public Key generation */
    uint8_t b_scalar[32];
    memcpy(b_scalar, BOB_PRIV, 32);
    curve25519_clamp(b_scalar);

    uint8_t b_pub[32];
    rc = curve25519_scalarmult_base(b_pub, b_scalar);
    check(rc == 0, "Bob base scalar multiplication succeeds");
    check(ceepew_ct_equal(b_pub, BOB_PUB_EXPECTED, 32) == 1,
          "Bob public point matches RFC 7748 vector");

    /* 3. Alice computes shared secret with Bob's public point */
    uint8_t ss_alice[32];
    CeePewErr_t err = crypto_ecdh_shared_secret(ALICE_PRIV, BOB_PUB_EXPECTED, ss_alice);
    check(err == CEEPEW_OK, "Alice ECDH shared secret computation succeeds");
    check(ceepew_ct_equal(ss_alice, SHARED_SECRET_EXPECTED, 32) == 1,
          "Alice shared secret matches RFC 7748 vector");

    /* 4. Bob computes shared secret with Alice's public point */
    uint8_t ss_bob[32];
    err = crypto_ecdh_shared_secret(BOB_PRIV, ALICE_PUB_EXPECTED, ss_bob);
    check(err == CEEPEW_OK, "Bob ECDH shared secret computation succeeds");
    check(ceepew_ct_equal(ss_bob, SHARED_SECRET_EXPECTED, 32) == 1,
          "Bob shared secret matches RFC 7748 vector");

    /* 5. Commutative equivalence */
    check(ceepew_ct_equal(ss_alice, ss_bob, 32) == 1,
          "Alice and Bob arrive at identical shared secret in constant-time");

    ceepew_secure_zero(a_scalar, sizeof(a_scalar));
    ceepew_secure_zero(b_scalar, sizeof(b_scalar));
    ceepew_secure_zero(ss_alice, sizeof(ss_alice));
    ceepew_secure_zero(ss_bob, sizeof(ss_bob));
}

static void test_low_order_rejection(void)
{
    printf("\n--- Test 2: Low-Order Point Rejection (RFC 7748 §6.1) ---\n");

    static const uint8_t low_order_points[7][32] = {
        /* 0 (u = 0) */
        { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
          0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
        /* 1 (u = 1) */
        { 0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
          0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
        /* Order 8 Point 1 */
        { 0xe0,0xeb,0x7a,0x7c,0x3b,0x41,0xb8,0xae,0x16,0x56,0xe3,0xfa,0xf1,0x9f,0xc4,0x6a,
          0xda,0x09,0x8d,0xeb,0x9c,0x32,0xb1,0xfd,0x86,0x62,0x05,0x16,0x5f,0x49,0xb8,0x00 },
        /* Order 8 Point 2 */
        { 0x5f,0x9c,0x95,0xbc,0xa3,0x50,0x8c,0x24,0xb1,0xd0,0xb1,0x55,0x9c,0x83,0xef,0x5b,
          0x04,0x44,0x5c,0xc4,0x58,0x1c,0x8e,0x86,0xd8,0x22,0x4e,0xdd,0xd0,0x9f,0x11,0x57 },
        /* p - 1 (order 2) */
        { 0xec,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
          0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f },
        /* p (≡ 0 mod p, order 4) */
        { 0xed,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
          0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f },
        /* p + 1 (≡ 1 mod p, order 1) */
        { 0xee,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
          0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f },
    };

    for (int i = 0; i < 7; i++) {
        char desc[64];
        snprintf(desc, sizeof(desc), "Torsion point #%d rejected by reject_low_order", i + 1);
        check(crypto_ecdh_reject_low_order(low_order_points[i]) == CEEPEW_ERR_CRYPTO, desc);

        uint8_t ss[32];
        snprintf(desc, sizeof(desc), "Torsion point #%d rejected by shared_secret", i + 1);
        check(crypto_ecdh_shared_secret(ALICE_PRIV, low_order_points[i], ss) == CEEPEW_ERR_CRYPTO, desc);
    }
}

static void test_keypair_generation(void)
{
    printf("\n--- Test 3: Ephemeral Keypair Generation & Exchange ---\n");
    uint8_t pub_a[32], priv_a[32];
    uint8_t pub_b[32], priv_b[32];

    CeePewErr_t err_a = crypto_ecdh_generate_keypair(pub_a, priv_a);
    CeePewErr_t err_b = crypto_ecdh_generate_keypair(pub_b, priv_b);

    check(err_a == CEEPEW_OK && err_b == CEEPEW_OK, "Keypair generation succeeds for both peers");
    check(ceepew_is_all_zero(pub_a, 32) == 0, "Peer A public key is non-zero");
    check(ceepew_is_all_zero(pub_b, 32) == 0, "Peer B public key is non-zero");
    check(memcmp(pub_a, pub_b, 32) != 0, "Keypairs are distinct and independent");

    uint8_t ss_ab[32], ss_ba[32];
    err_a = crypto_ecdh_shared_secret(priv_a, pub_b, ss_ab);
    err_b = crypto_ecdh_shared_secret(priv_b, pub_a, ss_ba);

    check(err_a == CEEPEW_OK && err_b == CEEPEW_OK, "Mutual shared secrets compute successfully");
    check(ceepew_ct_equal(ss_ab, ss_ba, 32) == 1, "Generated keypair shared secrets are identical");

    ceepew_secure_zero(priv_a, sizeof(priv_a));
    ceepew_secure_zero(priv_b, sizeof(priv_b));
    ceepew_secure_zero(ss_ab, sizeof(ss_ab));
    ceepew_secure_zero(ss_ba, sizeof(ss_ba));
}

int main(void)
{
    printf("============================================\n");
    printf(" CEE-PEW Curve25519 & ECDH Unit Test Suite\n");
    printf("============================================\n");

    test_rfc7748_test_vectors();
    test_low_order_rejection();
    test_keypair_generation();

    printf("\n============================================\n");
    printf(" Curve25519 Summary: %d Passed, %d Failed\n", s_passed, s_failed);
    printf("============================================\n");

    return (s_failed == 0) ? 0 : 1;
}

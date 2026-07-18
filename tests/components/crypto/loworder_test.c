/* tests/components/crypto/loworder_test.c
 *
 * Verifies that crypto_ecdh_shared_secret() and crypto_ecdh_reject_low_order()
 * correctly reject all 7 known Curve25519 low-order u-coordinate encodings
 * (RFC 7748 §6.1 / libsodium x25519_ref10.c blocklist).
 *
 * Also verifies that ceepew_is_all_zero() works correctly.
 *
 * Invoked from integration_test_e2e.c via loworder_selftest_run().
 * Gated by CEEPEW_ENABLE_SELFTEST (defined in tests/CMakeLists.txt when
 * CONFIG_CEEPEW_DEVELOPMENT_MODE is enabled).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ceepew_assert.h"
#include "ceepew_security_utils.h"
#include "crypto_ecdh.h"

#ifdef CEEPEW_ENABLE_SELFTEST

/* The 7 known low-order Curve25519 u-coordinate encodings.
 * Source: libsodium x25519_ref10.c blocklist. */
static const uint8_t s_low_order_points[7][32] = {
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    { 0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    { 0xe0,0xeb,0x7a,0x7c,0x3b,0x41,0xb8,0xae,0x16,0x56,0xe3,0xfa,0xf1,0x9f,0xc4,0x6a,
      0xda,0x09,0x8d,0xeb,0x9c,0x32,0xb1,0xfd,0x86,0x62,0x05,0x16,0x5f,0x49,0xb8,0x00 },
    { 0x5f,0x9c,0x95,0xbc,0xa3,0x50,0x8c,0x24,0xb1,0xd0,0xb1,0x55,0x9c,0x83,0xef,0x5b,
      0x04,0x44,0x5c,0xc4,0x58,0x1c,0x8e,0x86,0xd8,0x22,0x4e,0xdd,0xd0,0x9f,0x11,0x57 },
    { 0xec,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f },
    { 0xed,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f },
    { 0xee,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f },
};

static int s_pass = 0;
static int s_fail = 0;

static void test_assert_ok(const char *label, int ok)
{
    if (ok) {
        printf("[PASS] %s\n", label);
        s_pass++;
    } else {
        printf("[FAIL] %s\n", label);
        s_fail++;
    }
}

static void test_reject_low_order_reject(void)
{
    printf("=== Test: crypto_ecdh_reject_low_order rejects all 7 points ===\n");

    for (int j = 0; j < 7; j++) {
        CeePewErr_t err = crypto_ecdh_reject_low_order(s_low_order_points[j]);
        char label[64];
        snprintf(label, sizeof(label), "reject_low_order[%d]", j);
        test_assert_ok(label, (err == CEEPEW_ERR_CRYPTO));
    }
}

static void test_reject_low_order_accept_valid(void)
{
    printf("=== Test: crypto_ecdh_reject_low_order accepts valid point ===\n");

    uint8_t pub[32], priv[32];
    CeePewErr_t err = crypto_ecdh_generate_keypair(pub, priv);
    if (err != CEEPEW_OK) {
        test_assert_ok("generate_keypair for valid test", 0);
        ceepew_secure_zero(priv, 32U);
        return;
    }

    err = crypto_ecdh_reject_low_order(pub);
    test_assert_ok("reject_low_order(valid_point) == CEEPEW_OK", (err == CEEPEW_OK));
    ceepew_secure_zero(priv, 32U);
}

static void test_ecdh_rejects_low_order_point(int idx)
{
    uint8_t pub[32], priv[32], ss[32];

    CeePewErr_t err = crypto_ecdh_generate_keypair(pub, priv);
    if (err != CEEPEW_OK) {
        char label[64];
        snprintf(label, sizeof(label), "ecdh_shared_secret(low[%d]) keygen", idx);
        test_assert_ok(label, 0);
        return;
    }

    err = crypto_ecdh_shared_secret(priv, s_low_order_points[idx], ss);
    {
        char label[64];
        snprintf(label, sizeof(label), "ecdh_shared_secret(low[%d]) == CEEPEW_ERR_CRYPTO", idx);
        test_assert_ok(label, (err == CEEPEW_ERR_CRYPTO));
    }

    ceepew_secure_zero(pub, 32U);
    ceepew_secure_zero(priv, 32U);
    ceepew_secure_zero(ss, 32U);
}

static void test_ecdh_works_with_valid_key(void)
{
    printf("=== Test: crypto_ecdh_shared_secret with valid keypair ===\n");

    uint8_t alice_pub[32], alice_priv[32];
    uint8_t bob_pub[32], bob_priv[32];
    uint8_t ss_alice[32], ss_bob[32];

    CeePewErr_t err = crypto_ecdh_generate_keypair(alice_pub, alice_priv);
    if (err != CEEPEW_OK) { test_assert_ok("alice keygen", 0); goto cleanup; }
    err = crypto_ecdh_generate_keypair(bob_pub, bob_priv);
    if (err != CEEPEW_OK) { test_assert_ok("bob keygen", 0); goto cleanup; }

    err = crypto_ecdh_shared_secret(alice_priv, bob_pub, ss_alice);
    test_assert_ok("alice shared_secret OK", (err == CEEPEW_OK));

    err = crypto_ecdh_shared_secret(bob_priv, alice_pub, ss_bob);
    test_assert_ok("bob shared_secret OK", (err == CEEPEW_OK));

    int match = (memcmp(ss_alice, ss_bob, 32) == 0);
    test_assert_ok("shared secrets match", match);

cleanup:
    ceepew_secure_zero(alice_priv, 32U);
    ceepew_secure_zero(bob_priv, 32U);
    ceepew_secure_zero(ss_alice, 32U);
    ceepew_secure_zero(ss_bob, 32U);
}

static void test_is_all_zero(void)
{
    printf("=== Test: ceepew_is_all_zero ===\n");

    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));
    test_assert_ok("all-zero buffer detected", ceepew_is_all_zero(buf, 32U));

    buf[0] = 1;
    test_assert_ok("non-zero buffer rejected", !ceepew_is_all_zero(buf, 32U));

    memset(buf, 0, sizeof(buf));
    buf[31] = 0x80;
    test_assert_ok("last-byte non-zero rejected", !ceepew_is_all_zero(buf, 32U));

    test_assert_ok("NULL ptr returns 0", !ceepew_is_all_zero(NULL, 32U));
    test_assert_ok("zero-length returns 1", ceepew_is_all_zero(buf, 0U));
}

void loworder_selftest_run(void)
{
    printf("CEEPEW: Running low-order point self-tests\n");

    s_pass = 0;
    s_fail = 0;

    test_is_all_zero();
    test_reject_low_order_reject();
    test_reject_low_order_accept_valid();
    test_ecdh_works_with_valid_key();

    for (int i = 0; i < 7; i++) {
        test_ecdh_rejects_low_order_point(i);
    }

    printf("CEEPEW: Low-order point tests: %d pass, %d fail\n", s_pass, s_fail);
    if (s_fail > 0) {
        printf("CEEPEW: LOW-ORDER TEST SUITE FAILED\n");
    }
}

#endif /* CEEPEW_ENABLE_SELFTEST */

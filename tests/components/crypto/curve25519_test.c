/* tests/components/crypto/curve25519_test.c
 *
 * Self-test for X25519 using RFC7748 test vectors. Invoked from
 * tests/main/integration_test_e2e.c:integration_tests_run_all() via
 * the curve25519_selftest_run() entry point. No constructor — the
 * diagnostic harness drives the call deterministically.
 *
 * The file is gated by CEEPEW_ENABLE_SELFTEST which is defined in
 * tests/CMakeLists.txt when CONFIG_CEEPEW_BUILD_TESTS is on. The
 * source lives in tests/ so it does NOT enter the production binary
 * (tests/CMakeLists.txt registers SRCS "" when the option is off).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "curve25519.h"
#include "ceepew_assert.h"
#include "crypto_box_wrap.h"
#include "crypto_ctx.h"
#include "crypto_ecdh.h"
#include "session_fsm.h"

#ifdef CEEPEW_ENABLE_SELFTEST

static int hex2bin(const char *hex, uint8_t *out, size_t outlen) {
    size_t hexlen = strlen(hex);
    if (hexlen != outlen * 2) return -1;
    for (size_t i = 0; i < outlen; i++) {
        char a = hex[2*i];
        char b = hex[2*i+1];
        uint8_t va = (a >= '0' && a <= '9') ? (a - '0') : (a >= 'a' && a <= 'f') ? (10 + a - 'a') : (a >= 'A' && a <= 'F') ? (10 + a - 'A') : 255;
        uint8_t vb = (b >= '0' && b <= '9') ? (b - '0') : (b >= 'a' && b <= 'f') ? (10 + b - 'a') : (b >= 'A' && b <= 'F') ? (10 + b - 'A') : 255;
        if (va == 255 || vb == 255) return -2;
        out[i] = (uint8_t)((va << 4) | vb);
    }
    return 0;
}

static void run_vector(const char *label, const char *k_hex, const char *u_hex, const char *expected_hex) {
    uint8_t k[32], u[32], out[32], expected[32];
    int rc = hex2bin(k_hex, k, 32);
    if (rc) { printf("%s: invalid scalar hex\n", label); return; }
    rc = hex2bin(u_hex, u, 32);
    if (rc) { printf("%s: invalid u hex\n", label); return; }
    rc = hex2bin(expected_hex, expected, 32);
    if (rc) { printf("%s: invalid expected hex\n", label); return; }

    /* curve25519_scalarmult clamps internally per RFC7748 §5.
       The test vectors provide raw private keys, so do NOT clamp here. */

    int r = curve25519_scalarmult(out, k, u);
    if (r != 0) {
        printf("%s: curve25519_scalarmult returned error %d\n", label, r);
        return;
    }

    if (memcmp(out, expected, 32) == 0) { printf("%s: PASS\n", label);}
    else {
        printf("%s: FAIL\n  got:    ", label);
        for (size_t i = 0; i < 32; i++) printf("%02x", out[i]);
        printf("\n  expect: ");
        for (size_t i = 0; i < 32; i++) printf("%02x", expected[i]);
        printf("\n");
    }
}

static void test_crypto_box_loopback(void) {
    printf("CEEPEW: Running crypto_box loopback test\n");

    CryptoCtx_t ctxA;
    CryptoCtx_t ctxB;

    memset(&ctxA, 0, sizeof(ctxA));
    memset(&ctxB, 0, sizeof(ctxB));

    ctxA.session_active = true;
    ctxB.session_active = true;

    /* Generate keypair for A */
    CeePewErr_t err = crypto_ecdh_generate_keypair(ctxA.box_pubkey, ctxA.box_privkey);
    if (err != CEEPEW_OK) { printf("A keygen failed: %d\n", err); return; }

    /* Generate keypair for B */
    err = crypto_ecdh_generate_keypair(ctxB.box_pubkey, ctxB.box_privkey);
    if (err != CEEPEW_OK) { printf("B keygen failed: %d\n", err); return; }

    /* Exchange public keys */
    memcpy(ctxA.peer_box_pubkey, ctxB.box_pubkey, 32U);
    ctxA.peer_box_pubkey_valid = true;

    memcpy(ctxB.peer_box_pubkey, ctxA.box_pubkey, 32U);
    ctxB.peer_box_pubkey_valid = true;

    /* Set matching session IDs */
    uint8_t dummy_sid[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    memcpy(ctxA.session_id, dummy_sid, 8U);
    memcpy(ctxB.session_id, dummy_sid, 8U);

    /* Encrypt a test message from A to B */
    const uint8_t plaintext[] = "Hello World! This is a test message.";
    uint16_t pt_len = (uint16_t)strlen((char *)plaintext);

    uint8_t ciphertext[128];
    uint16_t ct_len = sizeof(ciphertext);

    /* Set nonce counter override for A encrypt */
    uint64_t nonce_ctr = 42ULL;
    session_test_set_nonce_counter(nonce_ctr);

    err = crypto_box_encrypt(&ctxA, ctxA.peer_box_pubkey, plaintext, pt_len, ciphertext, &ct_len);
    if (err != CEEPEW_OK) {
        printf("crypto_box_encrypt failed: %d\n", err);
        return;
    }

    /* Build the nonce that B expects */
    uint8_t nonce[CRYPTO_BOX_NONCEBYTES];
    memcpy(nonce, ctxB.session_id, 8U);
    for (uint8_t i = 0U; i < 8U; i++) {
        nonce[8U + i] = (uint8_t)((nonce_ctr >> (8U * i)) & 0xFFU);
    }
    memset(nonce + 16U, 0, 8U);

    /* Decrypt at B */
    uint8_t decrypted[128];
    uint16_t dec_len = sizeof(decrypted);
    err = crypto_box_decrypt(&ctxB, nonce, ctxB.peer_box_pubkey, ciphertext, ct_len, decrypted, &dec_len);
    if (err != CEEPEW_OK) {
        printf("crypto_box_decrypt failed: %d\n", err);
        return;
    }

    decrypted[dec_len] = '\0';
    if (dec_len == pt_len && memcmp(decrypted, plaintext, pt_len) == 0) {
        printf("crypto_box loopback: PASS (%s)\n", decrypted);
    } else {
        printf("crypto_box loopback: FAIL (len=%u got='%s')\n", dec_len, decrypted);
    }
}

void curve25519_selftest_run(void) {
    printf("CEEPEW: Running X25519 RFC7748 self-tests\n");

    /* RFC7748 test vector 1 */
    run_vector("X25519-TV-1", "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");

    /* RFC7748 test vector 2 */
    run_vector("X25519-TV-2", "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");

    test_crypto_box_loopback();

    printf("CEEPEW: X25519 self-tests complete\n");
}

#endif /* CEEPEW_ENABLE_SELFTEST */

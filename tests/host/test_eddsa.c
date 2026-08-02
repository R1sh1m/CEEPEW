/* tests/host/test_eddsa.c — Ed25519 EdDSA host unit test */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "crypto_eddsa.h"
#include "ceepew_security_utils.h"

/* ── Self-consistent round-trip tests ────────────────────────────────── */

static int test_deterministic_keypair(void)
{
    /* Same seed must always produce same keypair */
    const uint8_t seed[32] = {
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60
    };
    uint8_t pk1[32], sk1[64];
    uint8_t pk2[32], sk2[64];

    CeePewErr_t e1 = crypto_eddsa_seeded_keypair(pk1, sk1, seed);
    CeePewErr_t e2 = crypto_eddsa_seeded_keypair(pk2, sk2, seed);
    if (e1 != CEEPEW_OK || e2 != CEEPEW_OK) {
        printf("FAIL: deterministic_keypair returned %d, %d\n", e1, e2);
        return 1;
    }
    if (memcmp(pk1, pk2, 32) != 0 || memcmp(sk1, sk2, 64) != 0) {
        printf("FAIL: deterministic_keypair produced different keys\n");
        return 1;
    }
    printf("PASS: deterministic keypair\n");
    return 0;
}

static int test_sign_verify_roundtrip(void)
{
    const uint8_t seed[32] = {
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60
    };
    uint8_t pk[32], sk[64];
    if (crypto_eddsa_seeded_keypair(pk, sk, seed) != CEEPEW_OK) {
        printf("FAIL: sign_verify_roundtrip keypair generation\n");
        return 1;
    }

    const uint8_t msg[] = "hello ed25519";
    uint8_t sig[64];
    if (crypto_eddsa_sign(sk, msg, (uint16_t)sizeof(msg), sig) != CEEPEW_OK) {
        printf("FAIL: sign_verify_roundtrip sign failed\n");
        return 1;
    }

    if (crypto_eddsa_verify(pk, msg, (uint16_t)sizeof(msg), sig) != CEEPEW_OK) {
        printf("FAIL: sign_verify_roundtrip verify failed\n");
        return 1;
    }

    printf("PASS: sign/verify round-trip\n");
    return 0;
}

static int test_tampered_sig_fails(void)
{
    const uint8_t seed[32] = {0};
    uint8_t pk[32], sk[64];
    if (crypto_eddsa_seeded_keypair(pk, sk, seed) != CEEPEW_OK) {
        printf("FAIL: tampered_sig keypair generation\n");
        return 1;
    }

    const uint8_t msg[] = "test message";
    uint8_t sig[64];
    if (crypto_eddsa_sign(sk, msg, (uint16_t)sizeof(msg), sig) != CEEPEW_OK) {
        printf("FAIL: tampered_sig sign failed\n");
        return 1;
    }

    sig[31] ^= 0x01U;
    if (crypto_eddsa_verify(pk, msg, (uint16_t)sizeof(msg), sig) == CEEPEW_OK) {
        printf("FAIL: tampered_sig accepted tampered signature\n");
        return 1;
    }

    printf("PASS: tampered signature rejected\n");
    return 0;
}

static int test_wrong_pubkey_fails(void)
{
    const uint8_t seed_a[32] = {1};
    const uint8_t seed_b[32] = {2};
    uint8_t pk_a[32], sk_a[64];
    uint8_t pk_b[32], sk_b[64];
    if (crypto_eddsa_seeded_keypair(pk_a, sk_a, seed_a) != CEEPEW_OK) return 1;
    if (crypto_eddsa_seeded_keypair(pk_b, sk_b, seed_b) != CEEPEW_OK) return 1;

    const uint8_t msg[] = "cross-key test";
    uint8_t sig[64];
    if (crypto_eddsa_sign(sk_a, msg, (uint16_t)sizeof(msg), sig) != CEEPEW_OK) {
        printf("FAIL: wrong_pubkey sign failed\n");
        return 1;
    }

    if (crypto_eddsa_verify(pk_b, msg, (uint16_t)sizeof(msg), sig) == CEEPEW_OK) {
        printf("FAIL: wrong_pubkey verified with wrong public key\n");
        return 1;
    }

    printf("PASS: wrong public key rejected\n");
    return 0;
}

static int test_empty_message(void)
{
    const uint8_t seed[32] = {0};
    uint8_t pk[32], sk[64];
    if (crypto_eddsa_seeded_keypair(pk, sk, seed) != CEEPEW_OK) {
        printf("FAIL: empty_message keypair generation\n");
        return 1;
    }

    uint8_t sig[64];
    if (crypto_eddsa_sign(sk, NULL, 0U, sig) != CEEPEW_OK) {
        printf("FAIL: empty_message sign failed\n");
        return 1;
    }

    if (crypto_eddsa_verify(pk, NULL, 0U, sig) != CEEPEW_OK) {
        printf("FAIL: empty_message verify failed\n");
        return 1;
    }

    printf("PASS: empty message sign/verify\n");
    return 0;
}

static int test_deterministic_signature(void)
{
    const uint8_t seed[32] = {0xAA};
    uint8_t pk[32], sk[64];
    if (crypto_eddsa_seeded_keypair(pk, sk, seed) != CEEPEW_OK) {
        printf("FAIL: deterministic_sig keypair generation\n");
        return 1;
    }

    const uint8_t msg[] = "deterministic test";
    uint8_t sig1[64], sig2[64];
    if (crypto_eddsa_sign(sk, msg, (uint16_t)sizeof(msg), sig1) != CEEPEW_OK) return 1;
    if (crypto_eddsa_sign(sk, msg, (uint16_t)sizeof(msg), sig2) != CEEPEW_OK) return 1;

    if (memcmp(sig1, sig2, 64) != 0) {
        printf("FAIL: deterministic_sig produced different signatures\n");
        return 1;
    }

    printf("PASS: deterministic signature\n");
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += test_deterministic_keypair();
    failures += test_sign_verify_roundtrip();
    failures += test_tampered_sig_fails();
    failures += test_wrong_pubkey_fails();
    failures += test_empty_message();
    failures += test_deterministic_signature();
    printf("\nEd25519 EdDSA: %d failures\n", failures);
    return failures;
}

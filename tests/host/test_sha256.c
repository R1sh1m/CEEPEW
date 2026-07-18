/* tests/host/test_sha256.c — SHA-256 host unit test */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ceepew_assert.h"

/* Forward declaration */
CeePewErr_t crypto_sha256_compute(const uint8_t *in, uint32_t len, uint8_t out[32]);

static int test_sha256_known_vector(void)
{
    uint8_t digest[32];
    const uint8_t abc[3] = { 0x61, 0x62, 0x63 };

    CeePewErr_t err = crypto_sha256_compute(abc, 3, digest);
    if (err != CEEPEW_OK) {
        printf("FAIL: SHA-256 compute returned error %d\n", (int)err);
        return 1;
    }

    /* SHA-256("abc") = ba7816bf... */
    static const uint8_t abc_hash[32] = {
        0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
        0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
        0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
        0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD
    };

    if (memcmp(digest, abc_hash, 32) != 0) {
        printf("FAIL: SHA-256(\"abc\") mismatch\n");
        return 1;
    }

    printf("PASS: SHA-256(\"abc\") = \"ba7816bf...\"\n");
    return 0;
}

static int test_sha256_deterministic(void)
{
    const uint8_t data[128] = { 0 };
    uint8_t d1[32], d2[32];

    CeePewErr_t err = crypto_sha256_compute(data, sizeof(data), d1);
    if (err != CEEPEW_OK) return 1;

    err = crypto_sha256_compute(data, sizeof(data), d2);
    if (err != CEEPEW_OK) return 1;

    if (memcmp(d1, d2, 32) != 0) {
        printf("FAIL: SHA-256 not deterministic\n");
        return 1;
    }

    printf("PASS: SHA-256 deterministic\n");
    return 0;
}

static int test_sha256_differs(void)
{
    uint8_t a[32] = { 0x00 };
    uint8_t b[32] = { 0x01 };
    uint8_t da[32], db[32];

    CeePewErr_t err = crypto_sha256_compute(a, sizeof(a), da);
    if (err != CEEPEW_OK) return 1;

    err = crypto_sha256_compute(b, sizeof(b), db);
    if (err != CEEPEW_OK) return 1;

    if (memcmp(da, db, 32) == 0) {
        printf("FAIL: different inputs produced same hash\n");
        return 1;
    }

    printf("PASS: SHA-256 different inputs differ\n");
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += test_sha256_known_vector();
    failures += test_sha256_deterministic();
    failures += test_sha256_differs();
    printf("\nSHA-256: %d failures\n", failures);
    return failures;
}

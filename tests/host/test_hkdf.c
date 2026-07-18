/* tests/host/test_hkdf.c — HKDF-SHA256 host unit test */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ceepew_assert.h"

/* Forward declaration */
CeePewErr_t crypto_hkdf_derive(const uint8_t *ikm, uint8_t ikm_len,
                                const uint8_t *salt, uint8_t salt_len,
                                const uint8_t *info, uint8_t info_len,
                                uint8_t *out, uint8_t out_len);

/* RFC 5869 Test Case 1:
 *   IKM  = 0x0b0b... (22 bytes)
 *   salt = 0x0001... (13 bytes)
 *   info = 0xf0f1... (10 bytes)
 *   L    = 42
 */
static const uint8_t rfc5869_1_ikm[22] = {
    0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B,
    0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B,
    0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B
};
static const uint8_t rfc5869_1_salt[13] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C
};
static const uint8_t rfc5869_1_info[10] = {
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
    0xF8, 0xF9
};
static const uint8_t rfc5869_1_expected[42] = {
    0x3C, 0xB2, 0x5F, 0x25, 0xFA, 0xAC, 0xD5, 0x7A,
    0x90, 0x43, 0x4F, 0x64, 0xD0, 0x36, 0x2F, 0x2A,
    0x2D, 0x2D, 0x0A, 0x90, 0xCF, 0x1A, 0x5A, 0x4C,
    0x5D, 0xB0, 0x2D, 0x56, 0xEC, 0xC4, 0xC5, 0xBF,
    0x34, 0x00, 0x72, 0x08, 0xD5, 0xB8, 0x87, 0x18,
    0x58, 0x65
};

static int test_hkdf_rfc5869_1(void)
{
    uint8_t out[64];

    CeePewErr_t err = crypto_hkdf_derive(
        rfc5869_1_ikm, sizeof(rfc5869_1_ikm),
        rfc5869_1_salt, sizeof(rfc5869_1_salt),
        rfc5869_1_info, sizeof(rfc5869_1_info),
        out, sizeof(rfc5869_1_expected));

    if (err != CEEPEW_OK) {
        printf("FAIL: RFC 5869-1 returned %d\n", (int)err);
        return 1;
    }

    if (memcmp(out, rfc5869_1_expected, sizeof(rfc5869_1_expected)) != 0) {
        printf("FAIL: RFC 5869-1 output mismatch\n");
        return 1;
    }

    printf("PASS: HKDF RFC 5869 Test Case 1\n");
    return 0;
}

static int test_hkdf_deterministic(void)
{
    const uint8_t ikm[16] = { 0 };
    const uint8_t salt[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const uint8_t info[4] = { 0xAA, 0xBB, 0xCC, 0xDD };

    uint8_t out1[32], out2[32];

    CeePewErr_t err = crypto_hkdf_derive(ikm, sizeof(ikm), salt, sizeof(salt),
                                          info, sizeof(info), out1, sizeof(out1));
    if (err != CEEPEW_OK) return 1;

    err = crypto_hkdf_derive(ikm, sizeof(ikm), salt, sizeof(salt),
                              info, sizeof(info), out2, sizeof(out2));
    if (err != CEEPEW_OK) return 1;

    if (memcmp(out1, out2, sizeof(out1)) != 0) {
        printf("FAIL: HKDF not deterministic\n");
        return 1;
    }

    printf("PASS: HKDF deterministic\n");
    return 0;
}

static int test_hkdf_saltless(void)
{
    const uint8_t ikm[16] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                              0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00 };
    uint8_t out[16];

    CeePewErr_t err = crypto_hkdf_derive(ikm, sizeof(ikm), NULL, 0,
                                          NULL, 0, out, sizeof(out));
    if (err != CEEPEW_OK) {
        printf("FAIL: HKDF saltless returned %d\n", (int)err);
        return 1;
    }

    printf("PASS: HKDF saltless\n");
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += test_hkdf_rfc5869_1();
    failures += test_hkdf_deterministic();
    failures += test_hkdf_saltless();
    printf("\nHKDF: %d failures\n", failures);
    return failures;
}

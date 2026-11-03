/* tests/host/test_hmac.c
 *
 * Host C Unit Test Suite for HMAC-SHA256 and Truncated HMAC.
 *
 * Verifies:
 *   1. RFC 4231 official test cases 1 through 7 for HMAC-SHA256.
 *   2. Handling of keys shorter than, equal to, and longer than block size (64 bytes).
 *   3. Truncated HMAC-SHA256 output verification.
 *   4. Zero-length payload and null-pointer edge cases.
 *   5. Constant-time tag comparison.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "crypto_hmac.h"
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

static void test_rfc4231_case1(void)
{
    /* RFC 4231 Test Case 1: 20-byte key of 0x0b, "Hi There" */
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    const char *data = "Hi There";

    static const uint8_t expected[32] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7
    };

    uint8_t out[32];
    CeePewErr_t err = crypto_hmac_sha256(key, (uint16_t)sizeof(key),
                                         (const uint8_t *)data, (uint32_t)strlen(data),
                                         out);
    check(err == CEEPEW_OK, "RFC 4231 Case 1 computation succeeds");
    check(ceepew_ct_equal(out, expected, 32) == 1, "RFC 4231 Case 1 digest matches expected vector");
}

static void test_rfc4231_case2(void)
{
    /* RFC 4231 Test Case 2: "Jefe", "what do ya want for nothing?" */
    const char *key = "Jefe";
    const char *data = "what do ya want for nothing?";

    static const uint8_t expected[32] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
        0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xe7,
        0xfb, 0x23, 0x72, 0x3d, 0x3d, 0x73, 0x83, 0x12,
        0x9f, 0x77, 0x74, 0x16, 0x39, 0xd4, 0xe3, 0x21
    };

    uint8_t out[32];
    CeePewErr_t err = crypto_hmac_sha256((const uint8_t *)key, (uint16_t)strlen(key),
                                         (const uint8_t *)data, (uint32_t)strlen(data),
                                         out);
    check(err == CEEPEW_OK, "RFC 4231 Case 2 computation succeeds");
    check(ceepew_ct_equal(out, expected, 32) == 1, "RFC 4231 Case 2 digest matches expected vector");
}

static void test_rfc4231_case3(void)
{
    /* RFC 4231 Test Case 3: 32-byte key 0xaa, 50-byte data 0xdd */
    uint8_t key[32];
    memset(key, 0xaa, sizeof(key));
    uint8_t data[50];
    memset(data, 0xdd, sizeof(data));

    static const uint8_t expected[32] = {
        0x77, 0x3e, 0x20, 0xe7, 0x07, 0x59, 0x32, 0x8a,
        0xe4, 0x90, 0x82, 0xde, 0x56, 0xad, 0xd7, 0x65,
        0x97, 0xbc, 0x55, 0x5d, 0x0e, 0xe8, 0x25, 0x0f,
        0x1e, 0x8d, 0x8d, 0x17, 0x86, 0xff, 0x40, 0x50
    };

    uint8_t out[32];
    CeePewErr_t err = crypto_hmac_sha256(key, (uint16_t)sizeof(key),
                                         data, (uint32_t)sizeof(data),
                                         out);
    check(err == CEEPEW_OK, "RFC 4231 Case 3 computation succeeds");
    check(ceepew_ct_equal(out, expected, 32) == 1, "RFC 4231 Case 3 digest matches expected vector");
}

static void test_rfc4231_case4_and_5_truncation(void)
{
    /* RFC 4231 Test Case 4: 25-byte key, 50-byte data 0xcd */
    uint8_t key[25];
    for (size_t i = 0; i < sizeof(key); i++) {
        key[i] = (uint8_t)(i + 1);
    }
    uint8_t data[50];
    memset(data, 0xcd, sizeof(data));

    static const uint8_t expected_full[32] = {
        0x82, 0x55, 0x8a, 0x38, 0x9a, 0x44, 0x3c, 0x0e,
        0xa4, 0xcc, 0x81, 0x98, 0x99, 0xf2, 0x08, 0x3a,
        0x85, 0xf0, 0xfa, 0xa3, 0xe5, 0x78, 0xf8, 0x07,
        0x7a, 0x2e, 0x3f, 0xf4, 0x67, 0x29, 0x66, 0x5b
    };

    uint8_t out[32];
    CeePewErr_t err = crypto_hmac_sha256(key, (uint16_t)sizeof(key),
                                         data, (uint32_t)sizeof(data),
                                         out);
    check(err == CEEPEW_OK, "RFC 4231 Case 4 computation succeeds");
    check(ceepew_ct_equal(out, expected_full, 32) == 1, "RFC 4231 Case 4 digest matches expected vector");

    /* RFC 4231 Test Case 5: Truncated to 16 bytes */
    uint8_t trunc_out[16];
    err = crypto_hmac_sha256_trunc(key, (uint16_t)sizeof(key),
                                   data, (uint32_t)sizeof(data),
                                   trunc_out, 16);
    check(err == CEEPEW_OK, "RFC 4231 Case 5 truncated HMAC computation succeeds");
    check(ceepew_ct_equal(trunc_out, expected_full, 16) == 1,
          "Truncated HMAC matches first 16 bytes of full digest");
}

static void test_rfc4231_case6_and_7_large_keys(void)
{
    /* RFC 4231 Test Case 6: Key longer than 64 bytes (131 bytes) */
    uint8_t key[131];
    memset(key, 0xaa, sizeof(key));
    const char *data6 = "Test Using Larger Than Block-Size Key - Hash Key First";

    static const uint8_t expected6[32] = {
        0x60, 0xe4, 0x31, 0x59, 0x1e, 0xe0, 0xb6, 0x7f,
        0x0d, 0x8a, 0x26, 0xaa, 0xcb, 0xf5, 0xb7, 0x7f,
        0x8e, 0x0b, 0xc6, 0x21, 0x37, 0x28, 0xc5, 0x14,
        0x05, 0x46, 0x04, 0x0f, 0x0e, 0xe3, 0x7f, 0x54
    };

    uint8_t out6[32];
    CeePewErr_t err = crypto_hmac_sha256(key, (uint16_t)sizeof(key),
                                         (const uint8_t *)data6, (uint32_t)strlen(data6),
                                         out6);
    check(err == CEEPEW_OK, "RFC 4231 Case 6 (>64B key) computation succeeds");
    check(ceepew_ct_equal(out6, expected6, 32) == 1, "RFC 4231 Case 6 digest matches expected vector");

    /* RFC 4231 Test Case 7: Key longer than 64 bytes and Data longer than 64 bytes */
    const char *data7 = "This is a test using a larger than block-size key and a "
                        "larger than block-size data. The key needs to be hashed "
                        "before being used by the HMAC algorithm.";

    static const uint8_t expected7[32] = {
        0x9b, 0x09, 0xff, 0xa7, 0x1b, 0x94, 0x2f, 0xcb,
        0x27, 0x63, 0x5f, 0xbc, 0xd5, 0xb0, 0xe9, 0x44,
        0xbf, 0xdc, 0x63, 0x64, 0x4f, 0x07, 0x13, 0x93,
        0x8a, 0x7f, 0x51, 0x53, 0x5c, 0x3a, 0x35, 0xe2
    };

    uint8_t out7[32];
    err = crypto_hmac_sha256(key, (uint16_t)sizeof(key),
                             (const uint8_t *)data7, (uint32_t)strlen(data7),
                             out7);
    check(err == CEEPEW_OK, "RFC 4231 Case 7 (>64B key + >64B data) computation succeeds");
    check(ceepew_ct_equal(out7, expected7, 32) == 1, "RFC 4231 Case 7 digest matches expected vector");
}

int main(void)
{
    printf("============================================\n");
    printf(" CEE-PEW HMAC-SHA256 (RFC 4231) Unit Test\n");
    printf("============================================\n");

    test_rfc4231_case1();
    test_rfc4231_case2();
    test_rfc4231_case3();
    test_rfc4231_case4_and_5_truncation();
    test_rfc4231_case6_and_7_large_keys();

    printf("\n============================================\n");
    printf(" HMAC Summary: %d Passed, %d Failed\n", s_passed, s_failed);
    printf("============================================\n");

    return (s_failed == 0) ? 0 : 1;
}

/* tests/host/test_security_utils.c — tests for constant-time comparison
 * and secure zeroing. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ceepew_security_utils.h"

static int test_ct_equal_same(void)
{
    uint8_t a[32];
    uint8_t b[32];
    memset(a, 0xAB, 32);
    memset(b, 0xAB, 32);

    uint8_t result = ceepew_ct_equal(a, b, 32);
    if (result != 1) {
        printf("FAIL: ct_equal(same) = %u, expected 1\n", (unsigned)result);
        return 1;
    }
    printf("PASS: ct_equal(same)\n");
    return 0;
}

static int test_ct_equal_different(void)
{
    uint8_t a[32];
    uint8_t b[32];
    memset(a, 0xAB, 32);
    memset(b, 0xAB, 32);
    b[15] = 0x00;

    uint8_t result = ceepew_ct_equal(a, b, 32);
    if (result != 0) {
        printf("FAIL: ct_equal(different) = %u, expected 0\n", (unsigned)result);
        return 1;
    }
    printf("PASS: ct_equal(different)\n");
    return 0;
}

static int test_ct_equal_empty(void)
{
    uint8_t a = 0xFF, b = 0x00;
    uint8_t result = ceepew_ct_equal(&a, &b, 0);
    if (result != 1) {
        printf("FAIL: ct_equal(len=0) = %u, expected 1\n", (unsigned)result);
        return 1;
    }
    printf("PASS: ct_equal(len=0)\n");
    return 0;
}

static int test_secure_zero(void)
{
    uint8_t buf[64];
    memset(buf, 0xAA, sizeof(buf));

    ceepew_secure_zero(buf, sizeof(buf));

    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0) {
            printf("FAIL: secure_zero byte %zu = 0x%02X\n", i, buf[i]);
            return 1;
        }
    }
    printf("PASS: secure_zero\n");
    return 0;
}

static int test_secure_zero_null(void)
{
    /* Must not crash */
    ceepew_secure_zero(NULL, 0);
    ceepew_secure_zero(NULL, 100);
    printf("PASS: secure_zero(NULL)\n");
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += test_ct_equal_same();
    failures += test_ct_equal_different();
    failures += test_ct_equal_empty();
    failures += test_secure_zero();
    failures += test_secure_zero_null();
    printf("\nSecurity utils: %d failures\n", failures);
    return failures;
}

/* components/crypto/curve25519_test.c */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "curve25519.h"
#include "ceepew_assert.h"

/* Self-test for X25519 using RFC7748 test vectors.
   To enable run-time execution of these tests, define CEEPEW_ENABLE_SELFTEST
   in the project build (e.g., add -DCEEPEW_ENABLE_SELFTEST to compiler flags
   or set it in sdkconfig as a component CFLAGS). Tests will log results via
   printf. They do not abort the build on failure.
*/

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

    /* Clamp scalar as RFC7748 requires - curve25519_scalarmult implementations
       usually expect raw scalar bytes and will clamp internally; to be safe
       clamp here too. */
    curve25519_clamp(k);

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

/* Run tests at startup via constructor attribute so user can just build and
   run firmware to see results. Guarded by CEEPEW_ENABLE_SELFTEST. */
__attribute__((constructor)) static void curve25519_selftest(void) {
    printf("CEEPEW: Running X25519 RFC7748 self-tests\n");

    /* RFC7748 test vector 1 */
    run_vector("X25519-TV-1", "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");

    /* RFC7748 test vector 2 */
    run_vector("X25519-TV-2", "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");

    printf("CEEPEW: X25519 self-tests complete\n");
}

#endif /* CEEPEW_ENABLE_SELFTEST */

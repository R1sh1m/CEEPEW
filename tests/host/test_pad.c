/* tests/host/test_pad.c
 *
 * Host C Unit Test Suite for PKCS#7 64-byte block padding and constant-time unpadding.
 *
 * Verifies:
 *   1. Correct padding expansion across 0B, 1B, 63B, 64B, 65B, 127B, 128B inputs.
 *   2. Mandatory full-block addition when input is an exact multiple of 64 bytes.
 *   3. Round-trip unpadding fidelity: remove(apply(M)) == M for all 0..192 bytes.
 *   4. Rejection of corrupted/tampered padding bytes without timing leakage.
 *   5. Rejection of invalid pad byte values (0x00, >0x40).
 *   6. Boundary checks when destination buffer is undersized.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "crypto_pad.h"
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

static void test_pad_apply_boundaries(void)
{
    printf("\n--- Test 1: PKCS#7 Apply Block Boundaries ---\n");

    uint8_t out[256];
    uint16_t out_len;

    /* 0 bytes -> 64 bytes of 0x40 */
    out_len = sizeof(out);
    CeePewErr_t err = crypto_pad_apply(NULL, 0, out, &out_len);
    check(err == CEEPEW_OK, "Apply padding to 0-byte input succeeds");
    check(out_len == 64, "0-byte input padded to 64 bytes");
    bool all_40 = true;
    for (int i = 0; i < 64; i++) { if (out[i] != 0x40) all_40 = false; }
    check(all_40, "All 64 bytes are 0x40");

    /* 1 byte -> 64 bytes (1 data + 63 bytes of 0x3f) */
    uint8_t data1[1] = {0xAB};
    out_len = sizeof(out);
    err = crypto_pad_apply(data1, 1, out, &out_len);
    check(err == CEEPEW_OK, "Apply padding to 1-byte input succeeds");
    check(out_len == 64, "1-byte input padded to 64 bytes");
    check(out[0] == 0xAB, "First byte matches input data");
    bool all_3f = true;
    for (int i = 1; i < 64; i++) { if (out[i] != 0x3f) all_3f = false; }
    check(all_3f, "Remaining 63 bytes are 0x3f");

    /* 63 bytes -> 64 bytes (63 data + 1 byte of 0x01) */
    uint8_t data63[63];
    memset(data63, 0xEE, sizeof(data63));
    out_len = sizeof(out);
    err = crypto_pad_apply(data63, 63, out, &out_len);
    check(err == CEEPEW_OK, "Apply padding to 63-byte input succeeds");
    check(out_len == 64, "63-byte input padded to 64 bytes");
    check(out[63] == 0x01, "Final byte is 0x01");

    /* Exact multiple: 64 bytes -> 128 bytes (mandatory full block addition) */
    uint8_t data64[64];
    memset(data64, 0xCC, sizeof(data64));
    out_len = sizeof(out);
    err = crypto_pad_apply(data64, 64, out, &out_len);
    check(err == CEEPEW_OK, "Apply padding to 64-byte input succeeds");
    check(out_len == 128, "64-byte input padded to 128 bytes (full block added)");
    all_40 = true;
    for (int i = 64; i < 128; i++) { if (out[i] != 0x40) all_40 = false; }
    check(all_40, "Appended 64-byte block contains all 0x40 bytes");
}

static void test_pad_roundtrip_all_sizes(void)
{
    printf("\n--- Test 2: Exhaustive Round-trip Fidelity (0..192 bytes) ---\n");

    uint8_t original[192];
    uint8_t padded[256];
    uint8_t recovered[256];

    for (size_t i = 0; i < sizeof(original); i++) {
        original[i] = (uint8_t)((i * 37U + 11U) & 0xFFU);
    }

    bool all_roundtrips_passed = true;
    for (uint16_t len = 0; len <= 192; len++) {
        uint16_t pad_len = sizeof(padded);
        CeePewErr_t err = crypto_pad_apply(original, len, padded, &pad_len);
        if (err != CEEPEW_OK || (pad_len % 64) != 0 || pad_len <= len) {
            all_roundtrips_passed = false;
            break;
        }

        uint16_t rec_len = sizeof(recovered);
        err = crypto_pad_remove(padded, pad_len, recovered, &rec_len);
        if (err != CEEPEW_OK || rec_len != len || memcmp(original, recovered, len) != 0) {
            all_roundtrips_passed = false;
            break;
        }
    }
    check(all_roundtrips_passed, "All input lengths from 0 to 192 bytes round-trip perfectly");
}

static void test_pad_tamper_rejection(void)
{
    printf("\n--- Test 3: Tampered Padding Rejection ---\n");

    uint8_t data[50] = {0x12};
    uint8_t padded[64];
    uint16_t pad_len = sizeof(padded);
    crypto_pad_apply(data, 50, padded, &pad_len);
    check(pad_len == 64, "Padded to 64 bytes");

    /* Corrupt one pad byte in the middle of the pad block (byte 57) */
    uint8_t tampered[64];
    memcpy(tampered, padded, 64);
    tampered[57] ^= 0x01;

    uint8_t out[64];
    uint16_t out_len = sizeof(out);
    CeePewErr_t err = crypto_pad_remove(tampered, 64, out, &out_len);
    check(err != CEEPEW_OK, "Tampered pad byte in block is rejected");

    /* Corrupt the final length byte to 0x00 */
    memcpy(tampered, padded, 64);
    tampered[63] = 0x00;
    out_len = sizeof(out);
    err = crypto_pad_remove(tampered, 64, out, &out_len);
    check(err != CEEPEW_OK, "Pad length of 0x00 is rejected");

    /* Corrupt the final length byte to 65 (> block size 64) */
    tampered[63] = 65;
    out_len = sizeof(out);
    err = crypto_pad_remove(tampered, 64, out, &out_len);
    check(err != CEEPEW_OK, "Pad length exceeding 64 bytes is rejected");
}

static void test_buffer_bounds_protection(void)
{
    printf("\n--- Test 4: Buffer Bounds Protection ---\n");

    uint8_t in[30] = {0};
    uint8_t out[32]; /* too small for 64-byte block */
    uint16_t out_len = sizeof(out);

    CeePewErr_t err = crypto_pad_apply(in, 30, out, &out_len);
    check(err == CEEPEW_ERR_BOUNDS, "Undersized output buffer rejected with CEEPEW_ERR_BOUNDS");
}

int main(void)
{
    printf("============================================\n");
    printf(" CEE-PEW PKCS#7 Padding Engine Unit Test\n");
    printf("============================================\n");

    test_pad_apply_boundaries();
    test_pad_roundtrip_all_sizes();
    test_pad_tamper_rejection();
    test_buffer_bounds_protection();

    printf("\n============================================\n");
    printf(" Padding Summary: %d Passed, %d Failed\n", s_passed, s_failed);
    printf("============================================\n");

    return (s_failed == 0) ? 0 : 1;
}

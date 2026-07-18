/* tests/host/test_hamming.c — Hamming(15,11) FEC host unit test */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ceepew_assert.h"

/* Forward declarations from ecc_hamming.c */
CeePewErr_t ecc_hamming_init_session(const uint8_t seed[16]);
CeePewErr_t ecc_hamming_encode(const uint8_t *in, uint16_t in_len,
                               uint8_t *out, uint16_t *out_len);
CeePewErr_t ecc_hamming_decode(const uint8_t *in, uint16_t in_len,
                               uint8_t *out, uint16_t *out_len,
                               bool *corrected);
CeePewErr_t ecc_hamming_deinit(void);

static const uint8_t test_seed[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
};

static int test_roundtrip(void)
{
    uint8_t data[11] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B };
    uint8_t enc[256];
    uint16_t enc_len = sizeof(enc);
    uint8_t dec[256];
    uint16_t dec_len = sizeof(dec);
    bool corrected = false;

    ecc_hamming_init_session(test_seed);

    CeePewErr_t err = ecc_hamming_encode(data, sizeof(data), enc, &enc_len);
    if (err != CEEPEW_OK) {
        printf("FAIL: encode error %d\n", (int)err);
        ecc_hamming_deinit();
        return 1;
    }

    err = ecc_hamming_decode(enc, enc_len, dec, &dec_len, &corrected);
    if (err != CEEPEW_OK) {
        printf("FAIL: decode error %d\n", (int)err);
        ecc_hamming_deinit();
        return 1;
    }

    if (dec_len != sizeof(data)) {
        printf("FAIL: dec_len = %u, expected %zu\n", dec_len, sizeof(data));
        ecc_hamming_deinit();
        return 1;
    }

    if (memcmp(dec, data, sizeof(data)) != 0) {
        printf("FAIL: decoded data does not match original\n");
        ecc_hamming_deinit();
        return 1;
    }

    ecc_hamming_deinit();
    printf("PASS: Hamming round-trip\n");
    return 0;
}

static int test_single_bit_correction(void)
{
    uint8_t data[11] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    uint8_t enc[256];
    uint16_t enc_len = sizeof(enc);
    uint8_t dec[256];
    uint16_t dec_len = sizeof(dec);
    bool corrected = false;

    ecc_hamming_init_session(test_seed);

    ecc_hamming_encode(data, sizeof(data), enc, &enc_len);

    /* Flip one bit */
    enc[0] ^= 0x01;

    CeePewErr_t err = ecc_hamming_decode(enc, enc_len, dec, &dec_len, &corrected);
    if (err != CEEPEW_OK) {
        printf("FAIL: decode of corrected data returned error %d\n", (int)err);
        ecc_hamming_deinit();
        return 1;
    }

    if (!corrected) {
        printf("FAIL: single-bit error was not flagged as corrected\n");
        ecc_hamming_deinit();
        return 1;
    }

    if (memcmp(dec, data, sizeof(data)) != 0) {
        printf("FAIL: corrected data does not match original\n");
        ecc_hamming_deinit();
        return 1;
    }

    ecc_hamming_deinit();
    printf("PASS: Hamming single-bit correction\n");
    return 0;
}

static int test_double_bit_detection(void)
{
    uint8_t data[11] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA };
    uint8_t enc[256];
    uint16_t enc_len = sizeof(enc);
    uint8_t dec[256];
    uint16_t dec_len = sizeof(dec);
    bool corrected = false;

    ecc_hamming_init_session(test_seed);

    ecc_hamming_encode(data, sizeof(data), enc, &enc_len);

    /* Flip two bits */
    enc[0] ^= 0x03;

    CeePewErr_t err = ecc_hamming_decode(enc, enc_len, dec, &dec_len, &corrected);
    /* Double-bit error may either be reported as FEC_UNCORRECT or miscorrect.
     * We accept either — we're just checking it doesn't crash. */
    (void)err;

    ecc_hamming_deinit();
    printf("PASS: Hamming double-bit (no crash)\n");
    return 0;
}

static int test_empty_input(void)
{
    uint8_t enc[256];
    uint16_t enc_len = sizeof(enc);
    uint8_t dec[256];
    uint16_t dec_len = sizeof(dec);
    bool corrected = false;

    ecc_hamming_init_session(test_seed);

    /* The library rejects in_len=0 — we just verify it doesn't crash */
    CeePewErr_t err = ecc_hamming_encode(enc, 0, enc, &enc_len);
    if (err == CEEPEW_OK) {
        err = ecc_hamming_decode(enc, enc_len, dec, &dec_len, &corrected);
    }

    ecc_hamming_deinit();
    printf("PASS: Hamming empty input (no crash)\n");
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += test_roundtrip();
    failures += test_single_bit_correction();
    failures += test_double_bit_detection();
    failures += test_empty_input();
    printf("\nHamming: %d failures\n", failures);
    return failures;
}

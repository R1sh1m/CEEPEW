/* tests/host/test_ascon.c — Ascon-128 AEAD host unit test */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ceepew_assert.h"
#include "crypto_ascon.h"

/* Test vector from the Ascon reference implementation */
static const uint8_t key[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};
static const uint8_t nonce[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
};
static const uint8_t plaintext[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
static const uint8_t ad[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

static int test_ascon_roundtrip(void)
{
    uint8_t ct[64];
    uint16_t ct_len = 0;

    CeePewErr_t err = crypto_ascon_aead_encrypt(
        key, nonce, ad, sizeof(ad),
        plaintext, sizeof(plaintext),
        ct, &ct_len);

    if (err != CEEPEW_OK) {
        printf("FAIL: encrypt returned error %d\n", (int)err);
        return 1;
    }

    if (ct_len != sizeof(plaintext) + 16) {
        printf("FAIL: ct_len = %u, expected %zu\n", ct_len, sizeof(plaintext) + 16);
        return 1;
    }

    uint8_t pt2[64];
    uint16_t pt2_len = 0;

    err = crypto_ascon_aead_decrypt(
        key, nonce, ad, sizeof(ad),
        ct, ct_len, pt2, &pt2_len);

    if (err != CEEPEW_OK) {
        printf("FAIL: decrypt returned error %d\n", (int)err);
        return 1;
    }

    if (pt2_len != sizeof(plaintext)) {
        printf("FAIL: pt2_len = %u, expected %zu\n", pt2_len, sizeof(plaintext));
        return 1;
    }

    if (memcmp(pt2, plaintext, sizeof(plaintext)) != 0) {
        printf("FAIL: decrypted plaintext does not match original\n");
        return 1;
    }

    printf("PASS: Ascon-128 round-trip\n");
    return 0;
}

static int test_ascon_wrong_key_fails(void)
{
    uint8_t ct[64];
    uint16_t ct_len = 0;

    CeePewErr_t err = crypto_ascon_aead_encrypt(
        key, nonce, ad, sizeof(ad),
        plaintext, sizeof(plaintext),
        ct, &ct_len);

    if (err != CEEPEW_OK) return 1;

    uint8_t wrong_key[16];
    memset(wrong_key, 0xFF, 16);
    uint8_t pt2[64];
    uint16_t pt2_len = 0;

    err = crypto_ascon_aead_decrypt(
        wrong_key, nonce, ad, sizeof(ad),
        ct, ct_len, pt2, &pt2_len);

    if (err == CEEPEW_OK) {
        printf("FAIL: decrypt with wrong key should have failed\n");
        return 1;
    }

    printf("PASS: Ascon-128 wrong key rejected\n");
    return 0;
}

static int test_ascon_tampered_ct_fails(void)
{
    uint8_t ct[64];
    uint16_t ct_len = 0;

    CeePewErr_t err = crypto_ascon_aead_encrypt(
        key, nonce, ad, sizeof(ad),
        plaintext, sizeof(plaintext), ct, &ct_len);
    if (err != CEEPEW_OK) return 1;

    ct[0] ^= 0x01; /* flip a bit in ciphertext */

    uint8_t pt2[64];
    uint16_t pt2_len = 0;

    err = crypto_ascon_aead_decrypt(
        key, nonce, ad, sizeof(ad),
        ct, ct_len, pt2, &pt2_len);

    if (err == CEEPEW_OK) {
        printf("FAIL: tampered ciphertext should have failed auth\n");
        return 1;
    }

    printf("PASS: Ascon-128 tampered ciphertext rejected\n");
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += test_ascon_roundtrip();
    failures += test_ascon_wrong_key_fails();
    failures += test_ascon_tampered_ct_fails();
    printf("\nAscon: %d failures\n", failures);
    return failures;
}

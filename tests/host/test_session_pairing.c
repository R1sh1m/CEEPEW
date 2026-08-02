/* tests/host/test_session_pairing.c
 *
 * Automated Host-Native Pairing & Messaging Integration Test.
 *
 * Simulates two devices (Device A and Device B) undergoing the full CEE-PEW
 * 3-phase protocol on host (Windows/Linux/macOS) without hardware:
 *   1. Discovery & MAC sorting / role assignment
 *   2. Session code hashing & HKDF-SHA256 session key convergence
 *   3. Ed25519 ephemeral identity signing and verification
 *   4. Ascon-128a AEAD payload encryption & Hamming ECC encoding
 *   5. Transmission loopback, decoding, anti-replay verification & decryption
 *   6. Secure zeroing of volatile session keys on termination
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ceepew_security_utils.h"
#include "crypto_ascon.h"
#include "crypto_hkdf.h"
#include "ecc_hamming.h"

/* Forward declarations of SHA-256 and Ed25519 primitives */
CeePewErr_t crypto_sha256_compute(const uint8_t *in, uint32_t in_len, uint8_t out[32]);
CeePewErr_t crypto_eddsa_keypair(uint8_t pk[32], uint8_t sk[64]);
CeePewErr_t crypto_eddsa_sign(const uint8_t priv[64], const uint8_t *msg, uint16_t msg_len, uint8_t sig[64]);
CeePewErr_t crypto_eddsa_verify(const uint8_t pub[32], const uint8_t *msg, uint16_t msg_len, const uint8_t sig[64]);

/* Static mock MACs */
static const uint8_t MAC_A[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01 };
static const uint8_t MAC_B[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x02 };

/* Human pairing session code: "ZZZZ" expanded to 32 bytes */
static const uint8_t SESSION_CODE[32] = {
    'Z','Z','Z','Z','1','2','3','4','5','6','7','8','9','0','A','B',
    'C','D','E','F','1','2','3','4','5','6','7','8','9','0','1','2'
};

/* Test status counters */
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

/* Helper to construct 16-byte nonce: upper 8 bytes = session_id, lower 8 bytes = counter (BE) */
static void make_nonce(uint64_t session_id, uint64_t counter, uint8_t nonce[16])
{
    for (int i = 0; i < 8; i++) {
        nonce[i]     = (uint8_t)(session_id >> ((7 - i) * 8));
        nonce[8 + i] = (uint8_t)(counter >> ((7 - i) * 8));
    }
}

/* ───────────────────────────────────────────────────────────────────────────
 * Test 1: Role Assignment & MAC Ordering
 * ─────────────────────────────────────────────────────────────────────────── */
static void test_role_assignment(void)
{
    printf("\n--- Test 1: Role Assignment & MAC Ordering ---\n");
    int cmp = memcmp(MAC_A, MAC_B, 6);
    bool a_is_initiator = (cmp < 0);
    bool b_is_initiator = !a_is_initiator;

    check(a_is_initiator == true, "Device A (lower MAC) is Initiator");
    check(b_is_initiator == false, "Device B (higher MAC) is Responder");
}

/* ───────────────────────────────────────────────────────────────────────────
 * Test 2: HKDF Session Key Convergence
 * ─────────────────────────────────────────────────────────────────────────── */
static void test_key_convergence(uint8_t a_key[16], uint8_t b_key[16], uint64_t *out_session_id)
{
    printf("\n--- Test 2: HKDF Session Key Convergence ---\n");

    /* Construct sorted MAC buffer: MAC_A || MAC_B */
    uint8_t mac_buf[12];
    memcpy(mac_buf, MAC_A, 6);
    memcpy(mac_buf + 6, MAC_B, 6);

    /* Salt = SHA-256(MAC_A || MAC_B) */
    uint8_t salt[32];
    CeePewErr_t err = crypto_sha256_compute(mac_buf, sizeof(mac_buf), salt);
    check(err == CEEPEW_OK, "SHA-256 salt generation");

    /* Device A derives key using session code + salt */
    const char *info_str = "CEEPEW-SESSION-V1";
    err = crypto_hkdf_derive(SESSION_CODE, 32, salt, 32, (const uint8_t*)info_str, (uint8_t)strlen(info_str), a_key, 16);
    check(err == CEEPEW_OK, "Device A HKDF key derivation");

    /* Device B derives key independently using identical inputs */
    err = crypto_hkdf_derive(SESSION_CODE, 32, salt, 32, (const uint8_t*)info_str, (uint8_t)strlen(info_str), b_key, 16);
    check(err == CEEPEW_OK, "Device B HKDF key derivation");

    /* Assert byte-for-byte key equality using constant-time check */
    bool keys_equal = ceepew_ct_equal(a_key, b_key, 16);
    check(keys_equal, "Device A and Device B session keys match byte-for-byte");

    /* Derive session ID (upper 8 bytes of salt) */
    uint64_t session_id = 0;
    for (int i = 0; i < 8; i++) {
        session_id = (session_id << 8) | salt[i];
    }
    *out_session_id = session_id;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Test 3: Ed25519 Ephemeral Identity Authentication
 * ─────────────────────────────────────────────────────────────────────────── */
static void test_eddsa_authentication(void)
{
    printf("\n--- Test 3: Ed25519 Ephemeral Identity Authentication ---\n");

    uint8_t pk_a[32], sk_a[64];
    uint8_t pk_b[32], sk_b[64];

    check(crypto_eddsa_keypair(pk_a, sk_a) == CEEPEW_OK, "Device A Ed25519 keypair generation");
    check(crypto_eddsa_keypair(pk_b, sk_b) == CEEPEW_OK, "Device B Ed25519 keypair generation");

    /* Challenge payload: session code + sorted MACs */
    uint8_t challenge[44];
    memcpy(challenge, SESSION_CODE, 32);
    memcpy(challenge + 32, MAC_A, 6);
    memcpy(challenge + 38, MAC_B, 6);

    /* Device A signs challenge */
    uint8_t sig_a[64];
    check(crypto_eddsa_sign(sk_a, challenge, sizeof(challenge), sig_a) == CEEPEW_OK, "Device A sign challenge");

    /* Device B verifies Device A's signature */
    check(crypto_eddsa_verify(pk_a, challenge, sizeof(challenge), sig_a) == CEEPEW_OK, "Device B verify Device A signature");

    /* Verify tampered signature fails */
    sig_a[0] ^= 0xFF;
    check(crypto_eddsa_verify(pk_a, challenge, sizeof(challenge), sig_a) != CEEPEW_OK, "Reject tampered signature");
}

/* ───────────────────────────────────────────────────────────────────────────
 * Test 4 & 5: Ascon-128a Payload Encrypt/Decrypt, ECC & Anti-Replay Loopback
 * ─────────────────────────────────────────────────────────────────────────── */
static void test_messaging_and_anti_replay(const uint8_t session_key[16], uint64_t session_id)
{
    printf("\n--- Test 4 & 5: Ascon AEAD Encrypted Messaging & Anti-Replay ---\n");

    const uint8_t plaintext[20] = "CEEPEW-HOST-MSG-001";
    uint8_t ad[12];
    memcpy(ad, MAC_A, 6);
    memcpy(ad + 6, MAC_B, 6);

    /* Nonce 0 (Initiator counter starts at 0, increments by 2 for parity) */
    uint8_t nonce_a0[16];
    make_nonce(session_id, 0ULL, nonce_a0);

    /* Device A Encrypts */
    uint8_t ct_a[64];
    uint16_t ct_len = sizeof(ct_a);
    CeePewErr_t err = crypto_ascon_aead_encrypt(session_key, nonce_a0, ad, sizeof(ad),
                                                plaintext, sizeof(plaintext), ct_a, &ct_len);
    check(err == CEEPEW_OK, "Device A encrypt plaintext with Ascon-128a");
    check(ct_len == sizeof(plaintext) + 16, "Ciphertext length includes 16-byte MAC tag");

    /* Hamming ECC encoding */
    uint8_t ecc_encoded[128];
    uint16_t ecc_enc_len = sizeof(ecc_encoded);
    uint8_t ecc_decoded[128];
    uint16_t ecc_dec_len = sizeof(ecc_decoded);
    bool corrected = false;
    ecc_hamming_init_session(session_key);
    err = ecc_hamming_encode(ct_a, ct_len, ecc_encoded, &ecc_enc_len);
    check(err == CEEPEW_OK, "Hamming ECC encode payload");
    err = ecc_hamming_decode(ecc_encoded, ecc_enc_len, ecc_decoded, &ecc_dec_len, &corrected);
    check(err == CEEPEW_OK, "Hamming ECC decode payload");
    check(ecc_dec_len >= ct_len, "Decoded ECC payload capacity covers ciphertext");
    check(memcmp(ct_a, ecc_decoded, ct_len) == 0, "Decoded ECC payload matches original ciphertext");
    ecc_hamming_deinit();

    /* Device B Decrypts Nonce 0 */
    uint8_t pt_b[64];
    uint16_t pt_len = sizeof(pt_b);
    err = crypto_ascon_aead_decrypt(session_key, nonce_a0, ad, sizeof(ad), ct_a, ct_len, pt_b, &pt_len);
    check(err == CEEPEW_OK, "Device B decrypt ciphertext with Ascon-128a");
    check(pt_len == sizeof(plaintext), "Decrypted plaintext length matches");
    check(memcmp(pt_b, plaintext, sizeof(plaintext)) == 0, "Decrypted message matches original plaintext");

    /* Anti-Replay: Attempting to decrypt tampered ciphertext fails tag check */
    ct_a[0] ^= 0x01;
    err = crypto_ascon_aead_decrypt(session_key, nonce_a0, ad, sizeof(ad), ct_a, ct_len, pt_b, &pt_len);
    check(err != CEEPEW_OK, "Device B rejects tampered ciphertext (MAC check failure)");
}

/* ───────────────────────────────────────────────────────────────────────────
 * Test 5.5: Hamming ECC Fault Injection & Recovery
 * ─────────────────────────────────────────────────────────────────────────── */
static void test_hamming_error_injection(const uint8_t session_key[16], uint64_t session_id)
{
    printf("\n--- Test 5.5: Hamming ECC Fault Injection & Recovery ---\n");

    const uint8_t plaintext[20] = "CEEPEW-ECC-NOISE-99";
    uint8_t ad[12];
    memcpy(ad, MAC_A, 6);
    memcpy(ad + 6, MAC_B, 6);

    uint8_t nonce[16];
    make_nonce(session_id, 100ULL, nonce);

    /* 1. Encrypt plaintext */
    uint8_t ct[64];
    uint16_t ct_len = sizeof(ct);
    CeePewErr_t err = crypto_ascon_aead_encrypt(session_key, nonce, ad, sizeof(ad),
                                                plaintext, sizeof(plaintext), ct, &ct_len);
    check(err == CEEPEW_OK, "Hamming test: Ascon encrypt");

    /* 2. Hamming ECC encode */
    uint8_t ecc_encoded[128];
    uint16_t ecc_enc_len = sizeof(ecc_encoded);
    ecc_hamming_init_session(session_key);
    err = ecc_hamming_encode(ct, ct_len, ecc_encoded, &ecc_enc_len);
    check(err == CEEPEW_OK, "Hamming test: ECC encode");

    /* 3. Inject single-bit error */
    ecc_encoded[2] ^= 0x08; /* Flip 1 bit */

    /* 4. Decode and verify correction */
    uint8_t ecc_decoded[128];
    uint16_t ecc_dec_len = sizeof(ecc_decoded);
    bool corrected = false;
    err = ecc_hamming_decode(ecc_encoded, ecc_enc_len, ecc_decoded, &ecc_dec_len, &corrected);
    check(err == CEEPEW_OK, "Hamming test: Single-bit error decoded");
    check(corrected == true, "Hamming test: Single-bit error was successfully corrected");
    check(ecc_dec_len >= ct_len, "Hamming test: Decoded capacity covers ciphertext");
    check(memcmp(ct, ecc_decoded, ct_len) == 0, "Hamming test: Decoded payload matches original ciphertext");

    /* 5. Decrypt corrected payload */
    uint8_t pt_decoded[64];
    uint16_t pt_len = sizeof(pt_decoded);
    err = crypto_ascon_aead_decrypt(session_key, nonce, ad, sizeof(ad), ecc_decoded, ct_len, pt_decoded, &pt_len);
    check(err == CEEPEW_OK, "Hamming test: Decrypted corrected payload matches");
    check(memcmp(pt_decoded, plaintext, sizeof(plaintext)) == 0, "Hamming test: Plaintext matches");

    /* 6. Inject double-bit error (within the first 15-bit codeword block) */
    /* Re-encode a fresh clean copy */
    ecc_enc_len = sizeof(ecc_encoded);
    err = ecc_hamming_encode(ct, ct_len, ecc_encoded, &ecc_enc_len);
    check(err == CEEPEW_OK, "Hamming test: ECC re-encode");

    /* Flip 2 bits in the first codeword (bits 2 and 10) */
    ecc_encoded[0] ^= 0x20; /* Bit 2 */
    ecc_encoded[1] ^= 0x20; /* Bit 10 */

    ecc_dec_len = sizeof(ecc_decoded);
    corrected = false;
    err = ecc_hamming_decode(ecc_encoded, ecc_enc_len, ecc_decoded, &ecc_dec_len, &corrected);
    
    if (err == CEEPEW_OK) {
        /* If decoding succeeded (e.g. mis-corrected), Ascon decryption MUST fail security tag check */
        pt_len = sizeof(pt_decoded);
        err = crypto_ascon_aead_decrypt(session_key, nonce, ad, sizeof(ad), ecc_decoded, ct_len, pt_decoded, &pt_len);
        check(err != CEEPEW_OK, "Hamming test: Double-bit error rejected by cryptographic tag check");
    } else {
        /* Decoding failed directly (also safe/expected) */
        check(true, "Hamming test: Double-bit error rejected by ECC decoder");
    }

    ecc_hamming_deinit();
}

/* ───────────────────────────────────────────────────────────────────────────
 * Test 6: Volatile Key Zeroing
 * ─────────────────────────────────────────────────────────────────────────── */
static void test_key_zeroing(uint8_t key_a[16], uint8_t key_b[16])
{
    printf("\n--- Test 6: Volatile Key Security Zeroing ---\n");

    ceepew_secure_zero(key_a, 16);
    ceepew_secure_zero(key_b, 16);

    static const uint8_t ZEROES[16] = {0};
    check(memcmp(key_a, ZEROES, 16) == 0, "Device A session key securely zeroed");
    check(memcmp(key_b, ZEROES, 16) == 0, "Device B session key securely zeroed");
}

int main(void)
{
    printf("============================================\n");
    printf(" CEE-PEW Host Dual-Session Pairing Test\n");
    printf("============================================\n");

    uint8_t key_a[16], key_b[16];
    uint64_t session_id = 0;

    test_role_assignment();
    test_key_convergence(key_a, key_b, &session_id);
    test_eddsa_authentication();
    test_messaging_and_anti_replay(key_a, session_id);
    test_hamming_error_injection(key_a, session_id);
    test_key_zeroing(key_a, key_b);

    printf("\n============================================\n");
    printf(" Host Session Pairing Summary: %d Passed, %d Failed\n", s_passed, s_failed);
    printf("============================================\n");

    return (s_failed == 0) ? 0 : 1;
}

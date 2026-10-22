/* tests/host/test_fail_closed.c — ISS-S01 / ISS-S02 regression tests.
 *
 * The fail-closed peer-identity policy is expressed as small, pure, inline
 * predicates in main/session_fsm.h so that the policy can be exercised on
 * the host without an ESP32, a radio, or a BLE stack. These tests assert
 * the policy itself, not a mock of it:
 *
 *   1. Peer identity is verified only when all three facts hold
 *      (key received, canonical encoding, commitment bound).
 *   2. A missing, wrong-length, malformed, or unbound key never verifies.
 *   3. A degraded pairing can never reach ACTIVE without the explicit
 *      CONFIG_CEEPEW_ALLOW_UNAUTH_SESSION escape hatch.
 *   4. The transmit gate refuses to transmit under an unverified identity.
 *   5. The pairing code-space constants match the implementation
 *      (4 characters over a 36-symbol alphabet, 36^4 = 1 679 616).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "session_fsm.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("PASS: %s\n", (msg));                                      \
        } else {                                                              \
            printf("FAIL: %s\n", (msg));                                      \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

/* A canonical Ed25519 public key: y = 2 is a valid encoding (little-endian
 * 02 00 ... 00), well below the field prime. */
static void make_canonical_pk(uint8_t pk[32])
{
    memset(pk, 0, 32);
    pk[0] = 0x02U;
}

/* y = p (the field prime) is NOT a canonical encoding (RFC 8032 §5.1.3). */
static void make_noncanonical_pk(uint8_t pk[32])
{
    static const uint8_t p_le[32] = {
        0xEDU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x7FU
    };
    memcpy(pk, p_le, 32);
}

static void test_identity_predicate(void)
{
    SessionPeerIdentity_t id;

    memset(&id, 0, sizeof(id));
    CHECK(!session_peer_identity_is_verified(NULL),
          "identity predicate is fail-closed on a NULL record");
    CHECK(!session_peer_identity_is_verified(&id),
          "identity predicate rejects an all-false record");

    id.sign_pk_received = true;
    id.sign_pk_format_ok = true;
    id.commitment_bound = false;
    CHECK(!session_peer_identity_is_verified(&id),
          "identity predicate rejects an unbound commitment");

    id.sign_pk_received = false;
    id.sign_pk_format_ok = true;
    id.commitment_bound = true;
    CHECK(!session_peer_identity_is_verified(&id),
          "identity predicate rejects a missing key");

    id.sign_pk_received = true;
    id.sign_pk_format_ok = false;
    id.commitment_bound = true;
    CHECK(!session_peer_identity_is_verified(&id),
          "identity predicate rejects a malformed key");

    id.sign_pk_received = true;
    id.sign_pk_format_ok = true;
    id.commitment_bound = true;
    CHECK(session_peer_identity_is_verified(&id),
          "identity predicate accepts a fully verified peer");
}

static void test_key_format_and_length(void)
{
    uint8_t pk[32];

    make_canonical_pk(pk);
    CHECK(session_sign_pk_format_ok(pk),
          "canonical Ed25519 encoding is accepted");

    memset(pk, 0, 32);
    CHECK(!session_sign_pk_format_ok(pk),
          "all-zero public key is rejected");

    make_noncanonical_pk(pk);
    CHECK(!session_sign_pk_format_ok(pk),
          "non-canonical y = p is rejected");

    CHECK(!session_sign_pk_format_ok(NULL),
          "NULL public key is rejected");

    CHECK(session_peer_pk_length_ok(32U),
          "32-byte public key length is accepted");
    CHECK(!session_peer_pk_length_ok(31U),
          "31-byte public key length is rejected");
    CHECK(!session_peer_pk_length_ok(33U),
          "33-byte public key length is rejected");
    CHECK(!session_peer_pk_length_ok(0U),
          "zero-length public key is rejected");
}

static void test_degraded_never_activates(void)
{
    SessionPeerIdentity_t unverified;
    SessionPeerIdentity_t verified;

    memset(&unverified, 0, sizeof(unverified));
    memset(&verified, 0, sizeof(verified));
    verified.sign_pk_received = true;
    verified.sign_pk_format_ok = true;
    verified.commitment_bound = true;

    CHECK(!session_pairing_may_activate(&unverified, true, false),
          "degraded pairing cannot activate in a default build");
    CHECK(!session_pairing_may_activate(&unverified, false, false),
          "unverified pairing cannot activate in a default build");
    CHECK(!session_pairing_may_activate(NULL, true, false),
          "a NULL identity record cannot activate");
    CHECK(session_pairing_may_activate(&unverified, true, true),
          "the escape hatch admits a degraded pairing only when it is enabled "
          "AND the pairing is actually degraded");
    CHECK(!session_pairing_may_activate(&unverified, false, true),
          "the escape hatch does not admit a pairing that was never degraded");
    CHECK(session_pairing_may_activate(&verified, false, false),
          "a verified pairing activates without any escape hatch");
}

static void test_transmit_gate(void)
{
    CHECK(session_tx_identity_gate(true) == CEEPEW_OK,
          "transmit gate admits a verified identity");
    CHECK(session_tx_identity_gate(false) == CEEPEW_ERR_AUTH_FAIL,
          "transmit gate refuses an unverified identity");
}

static void test_code_space_constants(void)
{
    unsigned long computed = 1UL;

    for (unsigned i = 0U; i < CEEPEW_PAIRING_CODE_CHARS; i++) {
        computed *= CEEPEW_PAIRING_CODE_ALPHABET;
    }

    CHECK(CEEPEW_PAIRING_CODE_CHARS == 4U,
          "pairing code is 4 characters");
    CHECK(CEEPEW_PAIRING_CODE_ALPHABET == 36U,
          "pairing code alphabet is 36 symbols (0-9, A-Z)");
    CHECK(computed == CEEPEW_PAIRING_CODE_SPACE,
          "declared code space equals 36^4");
    CHECK(CEEPEW_PAIRING_CODE_SPACE == 1679616UL,
          "code space constant equals 1 679 616");
    CHECK(CEEPEW_PAIRING_MAX_ATTEMPTS > 0U,
          "a positive online attempt bound is declared");
}

int main(void)
{
    test_identity_predicate();
    test_key_format_and_length();
    test_degraded_never_activates();
    test_transmit_gate();
    test_code_space_constants();

    printf("\nFail-closed identity: %d failures\n", g_failures);
    return g_failures;
}

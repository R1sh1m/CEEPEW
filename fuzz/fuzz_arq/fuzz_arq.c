/* fuzz/fuzz_arq/fuzz_arq.c — ARQ seq/state logic fuzz target
 *
 * Fuzzes ecc_arq_encode() and ecc_arq_decode() by feeding arbitrary
 * byte sequences through the stop-and-wait state machine.
 *
 * Invariants:
 *   1. encode then decode of the same payload round-trips correctly
 *      when sequence numbers are in-order.
 *   2. decode rejects duplicate or out-of-order frames.
 *   3. reset clears all state (sequence begins again at 0).
 *
 * Build (libFuzzer):
 *   clang -fsanitize=fuzzer -I../../components/ecc -I../../components/ceepew_common \
 *         -I../../components/hal -I../../components/transport \
 *         -c ../../components/ecc/ecc_arq.c -o ecc_arq.o
 *   clang -fsanitize=fuzzer -I../../components/ecc -I../../components/ceepew_common \
 *         -I../../components/hal -I../../components/transport \
 *         fuzz_arq.c ecc_arq.o -o fuzz_arq
 *
 * Build (AFL):
 *   afl-clang-fast ... (same flags, link with __AFL_FUZZ_INIT)
 *
 * NOTE: ecc_arq.c depends on transport_espnow_send, transport_wait_ack, and
 * transport_espnow_rendezvous_drive which are referenced but not called from
 * ecc_arq_encode/ecc_arq_decode/ecc_arq_reset. Provide stubs below.
 */

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "ceepew_assert.h"

/* Stubs for transport functions referenced but not called by encode/decode/reset */
CeePewErr_t transport_espnow_send(const uint8_t *peer_mac, const uint8_t *data, uint16_t len) {
    (void)peer_mac; (void)data; (void)len; return 0;
}
CeePewErr_t transport_wait_ack(const uint8_t *peer_mac, uint16_t seq, uint32_t timeout_ms) {
    (void)peer_mac; (void)seq; (void)timeout_ms; return 0;
}
CeePewErr_t transport_espnow_rendezvous_drive(void) { return 0; }

/* ARQ functions to fuzz */
CeePewErr_t ecc_arq_encode(const uint8_t *in, uint16_t in_len,
                           uint8_t *out, uint16_t *out_len,
                           uint16_t max_out_len);
CeePewErr_t ecc_arq_decode(const uint8_t *in, uint16_t in_len,
                           uint8_t *out, uint16_t *out_len,
                           uint16_t max_out_len, bool *corrected);
CeePewErr_t ecc_arq_reset(void);

#ifndef CEEPEW_OK
#define CEEPEW_OK 0
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1U) return 0;

    /* Use first byte to decide the test scenario */
    uint8_t scenario = data[0];
    const uint8_t *payload = data + 1;
    uint16_t payload_len   = (uint16_t)(size - 1U);
    if (payload_len > 200U) payload_len = 200U;

    uint8_t frame[256];
    uint16_t frame_len = 0U;

    switch (scenario % 4U) {

    case 0: {
        /* Round-trip: encode then decode */
        ecc_arq_reset();
        CeePewErr_t err = ecc_arq_encode(payload, payload_len,
                                         frame, &frame_len, sizeof(frame));
        if (err != CEEPEW_OK) return 0;

        uint8_t out[256];
        uint16_t out_len = 0U;
        bool corrected = false;
        err = ecc_arq_decode(frame, frame_len, out, &out_len,
                             sizeof(out), &corrected);
        if (err != CEEPEW_OK) __builtin_trap();
        if (out_len != payload_len) __builtin_trap();
        if (memcmp(out, payload, payload_len) != 0) __builtin_trap();
        break;
    }

    case 1: {
        /* Duplicate rejection: encode once, decode twice */
        ecc_arq_reset();
        CeePewErr_t err = ecc_arq_encode(payload, payload_len,
                                         frame, &frame_len, sizeof(frame));
        if (err != CEEPEW_OK) return 0;

        uint8_t out[256];
        uint16_t out_len = 0U;
        bool corrected = false;
        err = ecc_arq_decode(frame, frame_len, out, &out_len,
                             sizeof(out), &corrected);
        if (err != CEEPEW_OK) return 0;

        /* Second decode of same frame must fail with replay */
        err = ecc_arq_decode(frame, frame_len, out, &out_len,
                             sizeof(out), &corrected);
        if (err == CEEPEW_OK) __builtin_trap();  /* replay not detected */
        break;
    }

    case 2: {
        /* Reset clears state: encode, decode, reset, encode again, decode */
        ecc_arq_reset();

        uint8_t f1[64]; uint16_t f1_len;
        CeePewErr_t err = ecc_arq_encode(payload, 1, f1, &f1_len, sizeof(f1));
        if (err != CEEPEW_OK) return 0;

        uint8_t out[64]; uint16_t out_len; bool corrected;
        err = ecc_arq_decode(f1, f1_len, out, &out_len, sizeof(out), &corrected);
        if (err != CEEPEW_OK) return 0;

        /* Reset and try again — should work from seq=0 again */
        ecc_arq_reset();
        uint8_t f2[64]; uint16_t f2_len;
        err = ecc_arq_encode(payload, 1, f2, &f2_len, sizeof(f2));
        if (err != CEEPEW_OK) return 0;

        err = ecc_arq_decode(f2, f2_len, out, &out_len, sizeof(out), &corrected);
        if (err != CEEPEW_OK) __builtin_trap();
        break;
    }

    case 3: {
        /* Feed raw fuzzer data directly to decode (negative testing).
         * Decode must not crash regardless of input — just return an error. */
        ecc_arq_reset();

        /* First, establish a valid starting state */
        uint8_t seed[] = { 0x00 };
        uint8_t f[4]; uint16_t fl;
        ecc_arq_encode(seed, 1, f, &fl, sizeof(f));

        uint8_t out[256]; uint16_t out_len; bool corrected;
        ecc_arq_decode(f, fl, out, &out_len, sizeof(out), &corrected);

        /* Now feed random junk — must not crash, just return error */
        (void)ecc_arq_decode(data, (uint16_t)(size < 512U ? size : 512U),
                             out, &out_len, sizeof(out), &corrected);
        break;
    }

    }

    return 0;
}

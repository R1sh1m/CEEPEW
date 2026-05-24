/* components/crypto/curve25519.h */

#ifndef CURVE25519_H
#define CURVE25519_H

#include <stdint.h>

/* Public-domain minimal Curve25519 (TweetNaCl-derived) API used by CEE-PEW.
   Provides scalar multiplication without using 128-bit intrinsics so it
   compiles with the xtensa-esp-elf toolchain.
*/

/* Compute q = scalar * p, where scalar and p are 32-byte little-endian
   values. Returns 0 on success. */
int curve25519_scalarmult(uint8_t q[32], const uint8_t scalar[32], const uint8_t p[32]);

/* Compute q = scalar * base_point (base point = 9). */
int curve25519_scalarmult_base(uint8_t q[32], const uint8_t scalar[32]);

/* Clamp scalar in-place according to X25519 spec. */
void curve25519_clamp(uint8_t scalar[32]);

#endif /* CURVE25519_H */

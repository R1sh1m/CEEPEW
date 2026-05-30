/* main/ceepew_security_utils.h */

#ifndef CEEPEW_SECURITY_UTILS_H
#define CEEPEW_SECURITY_UTILS_H

#include <stdint.h>

void ceepew_secure_zero(volatile void *ptr, uint32_t len);
uint8_t ceepew_ct_equal(const uint8_t *a, const uint8_t *b, uint32_t len);

/* Constant-time comparison for lexicographic ordering.
 * Returns 1 if a < b, 0 otherwise (constant-time).
 * Used for deterministic role selection in pairing (lower MAC = initiator).
 */
uint8_t ceepew_ct_less(const uint8_t *a, const uint8_t *b, uint32_t len);

#endif /* CEEPEW_SECURITY_UTILS_H */

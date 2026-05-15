/* mbedtls/hkdf.h - stub */

#ifndef MBEDTLS_HKDF_H
#define MBEDTLS_HKDF_H

#include <stddef.h>
#include "mbedtls/md.h"

int mbedtls_hkdf(const mbedtls_md_info_t *md,
                 const unsigned char *salt, size_t salt_len,
                 const unsigned char *ikm, size_t ikm_len,
                 const unsigned char *info, size_t info_len,
                 unsigned char *okm, size_t okm_len);

#endif /* MBEDTLS_HKDF_H */

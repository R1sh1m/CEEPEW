/* mbedtls/md.h - minimal stub */

#ifndef MBEDTLS_MD_H
#define MBEDTLS_MD_H

#include <stddef.h>

typedef struct mbedtls_md_info_t {
    int type;
    const char *name;
} mbedtls_md_info_t;

#define MBEDTLS_MD_SHA256 5

const mbedtls_md_info_t *mbedtls_md_info_from_type(int md_type);

#endif /* MBEDTLS_MD_H */

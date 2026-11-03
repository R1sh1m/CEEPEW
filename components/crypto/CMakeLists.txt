set(CRYPTO_SRCS
    "crypto_box_wrap.c"
    "crypto_ctx.c"
    "crypto_ascon.c"
    "crypto_ecdh.c"
    "crypto_eddsa.c"
    "crypto_hkdf.c"
    "crypto_sha256.c"
    "crypto_rng.c"
    "crypto_pad.c"
    "crypto_stream.c"
    "curve25519.c"
    "crypto_hmac.c"
    "crypto_hmac_efuse.c"
    "crypto_pake.c"
)

# The same-board cryptographic baseline harness (campaign X2) is only
# compiled into measurement builds. It is the only file in this component
# that needs the timer, and keeping it out of the shipping image keeps the
# production dependency graph unchanged.
if(CONFIG_CEEPEW_BENCH_AEAD)
    list(APPEND CRYPTO_SRCS "crypto_bench.c")
endif()

idf_component_register(
    SRCS ${CRYPTO_SRCS}
    INCLUDE_DIRS
        "."
    REQUIRES
        ceepew_common
        mem
        mbedtls
        esp_system
        esp_hw_support
        esp_timer
        efuse
        log
)

if(CONFIG_CEEPEW_BENCH_AEAD)
    # Enable PSA crypto defines so crypto_bench.c can use mbedTLS PSA APIs
    # (ESP-IDF v6.0.2 does not expose CONFIG_MBEDTLS_PSA_CRYPTO_C).
    target_compile_definitions(${COMPONENT_LIB} PRIVATE -DMBEDTLS_PSA_CRYPTO_C)
    # Also enable specific algorithms
    target_compile_definitions(${COMPONENT_LIB} PRIVATE -DMBEDTLS_GCM_C -DMBEDTLS_CCM_C -DMBEDTLS_CHACHAPOLY_C -DMBEDTLS_CHACHA20_C -DMBEDTLS_POLY1305_C)
endif()

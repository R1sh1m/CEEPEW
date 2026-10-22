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
        mbedtls
        esp_system
        esp_hw_support
        esp_timer
        efuse
        log
)

idf_component_register(
    SRCS
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
        "curve25519_test.c"
    INCLUDE_DIRS
        "."
        "../../main"
    REQUIRES
        ceepew_hal
        mbedtls
        esp_system
        log
)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CEEPEW_ENABLE_SELFTEST)
endif()

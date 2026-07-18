cmake_minimum_required(VERSION 3.16)
project(ceepew-host-tests C)

# ── Include paths ────────────────────────────────────────────────────
set(COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include              # stub ESP-IDF headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ceepew_common
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/hal
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/crypto
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ecc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/mem
)

# ── Source paths ─────────────────────────────────────────────────────
set(COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# ── Compile definitions (ESP-IDF stubs) ──────────────────────────────
add_compile_definitions(
    WIFI_PS_MIN_MODEM=0
    WIFI_PS_NONE=0
    CONFIG_CEEPEW_DEVELOPMENT_MODE=1
)

# ── Helper: portable component object library ────────────────────────
# Sources that are portable aside from esp_log.h (which we stub)
set(PORTABLE_SRCS
    ${COMPONENT_DIR}/components/ceepew_common/ceepew_security_utils.c
    ${COMPONENT_DIR}/components/crypto/crypto_ascon.c
    ${COMPONENT_DIR}/components/ecc/ecc_hamming.c
)

add_library(ceepew_portable OBJECT ${PORTABLE_SRCS})
target_include_directories(ceepew_portable PRIVATE ${COMMON_INCLUDES})

# ── mbedTLS-dependent library ────────────────────────────────────────
# crypto_sha256.c and crypto_hkdf.c require mbedtls/md.h
set(MBEDTLS_SRCS
    ${COMPONENT_DIR}/components/crypto/crypto_sha256.c
    ${COMPONENT_DIR}/components/crypto/crypto_hkdf.c
)

add_library(ceepew_mbedtls OBJECT ${MBEDTLS_SRCS})
target_include_directories(ceepew_mbedtls PRIVATE ${COMMON_INCLUDES})
target_link_libraries(ceepew_mbedtls PUBLIC mbedcrypto)

# ── Test executables ─────────────────────────────────────────────────

# test_security_utils — tests ceepew_ct_equal and ceepew_secure_zero
add_executable(test_security_utils
    ${CMAKE_CURRENT_SOURCE_DIR}/test_security_utils.c
)
target_link_libraries(test_security_utils PRIVATE ceepew_portable)
target_include_directories(test_security_utils PRIVATE ${COMMON_INCLUDES})

# test_ascon — tests crypto_ascon_aead encrypt/decrypt
add_executable(test_ascon
    ${CMAKE_CURRENT_SOURCE_DIR}/test_ascon.c
)
target_link_libraries(test_ascon PRIVATE ceepew_portable)
target_include_directories(test_ascon PRIVATE ${COMMON_INCLUDES})

# test_hamming — tests ecc_hamming encode/decode round-trip
add_executable(test_hamming
    ${CMAKE_CURRENT_SOURCE_DIR}/test_hamming.c
)
target_link_libraries(test_hamming PRIVATE ceepew_portable)
target_include_directories(test_hamming PRIVATE ${COMMON_INCLUDES})

# test_sha256 — tests crypto_sha256_compute
add_executable(test_sha256
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sha256.c
)
target_link_libraries(test_sha256 PRIVATE ceepew_mbedtls ceepew_portable)
target_link_libraries(test_sha256 PRIVATE mbedcrypto)
target_include_directories(test_sha256 PRIVATE ${COMMON_INCLUDES})

# test_hkdf — tests crypto_hkdf_derive
add_executable(test_hkdf
    ${CMAKE_CURRENT_SOURCE_DIR}/test_hkdf.c
)
target_link_libraries(test_hkdf PRIVATE ceepew_mbedtls ceepew_portable)
target_link_libraries(test_hkdf PRIVATE mbedcrypto)
target_include_directories(test_hkdf PRIVATE ${COMMON_INCLUDES})

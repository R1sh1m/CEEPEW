cmake_minimum_required(VERSION 3.16)
project(ceepew-host-tests C)
enable_testing()

# ── Include paths ────────────────────────────────────────────────────
set(COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include              # stub ESP-IDF headers
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ceepew_common
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/hal
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/crypto
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ecc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/mem
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/compress
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/transport
)

# ── Source paths ─────────────────────────────────────────────────────
set(COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# ── Compile definitions (ESP-IDF stubs) ──────────────────────────────
add_compile_definitions(
    WIFI_PS_MIN_MODEM=0
    WIFI_PS_NONE=0
    CONFIG_CEEPEW_DEVELOPMENT_MODE=1
)

option(CEEPEW_HOST_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" ON)
if(CEEPEW_HOST_SANITIZERS)
    if(NOT MINGW)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

option(CEEPEW_HOST_COVERAGE "Enable gcov code coverage generation" OFF)
if(CEEPEW_HOST_COVERAGE)
    add_compile_options(-fprofile-arcs -ftest-coverage)
    add_link_options(-fprofile-arcs -ftest-coverage)
endif()

# ── Helper: portable component object library ────────────────────────
# Sources that are portable aside from esp_log.h (which we stub)
set(PORTABLE_SRCS
    ${COMPONENT_DIR}/components/ceepew_common/ceepew_security_utils.c
    ${COMPONENT_DIR}/components/crypto/crypto_ascon.c
    ${COMPONENT_DIR}/components/ecc/ecc_hamming.c
    ${COMPONENT_DIR}/components/compress/compress_huffman.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/ceepew_assert_stub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/crypto_rng_stub.c
)

add_library(ceepew_portable OBJECT ${PORTABLE_SRCS})
target_include_directories(ceepew_portable PRIVATE ${COMMON_INCLUDES})

# ── mbedTLS-dependent library ────────────────────────────────────────
# crypto_sha256.c and crypto_hkdf.c require mbedtls/md.h
set(MBEDTLS_SRCS
    ${COMPONENT_DIR}/components/crypto/crypto_sha256.c
    ${COMPONENT_DIR}/components/crypto/crypto_hmac.c
    ${COMPONENT_DIR}/components/crypto/crypto_hkdf.c
    ${COMPONENT_DIR}/components/crypto/crypto_eddsa.c
)

add_library(ceepew_mbedtls OBJECT ${MBEDTLS_SRCS})
target_include_directories(ceepew_mbedtls PRIVATE ${COMMON_INCLUDES})
target_link_libraries(ceepew_mbedtls PUBLIC mbedcrypto)

# MinGW static linking workaround: -static avoids WDAC/DeviceGuard
# blocking executables that import from external DLLs.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    set(MBEDCRYPTO_STATIC_LIBS "-Wl,-Bstatic" mbedcrypto "-Wl,-Bdynamic" bcrypt)
else()
    set(MBEDCRYPTO_STATIC_LIBS mbedcrypto)
endif()

# ── Test executables ─────────────────────────────────────────────────

# test_security_utils — tests ceepew_ct_equal and ceepew_secure_zero
add_executable(test_security_utils
    ${CMAKE_CURRENT_SOURCE_DIR}/test_security_utils.c
)
target_link_libraries(test_security_utils PRIVATE ceepew_portable)
target_include_directories(test_security_utils PRIVATE ${COMMON_INCLUDES})
add_test(NAME test_security_utils COMMAND test_security_utils)

# test_side_channel — empirical constant-time verification harness (dudect)
add_executable(test_side_channel
    ${CMAKE_CURRENT_SOURCE_DIR}/test_side_channel.c
)
target_link_libraries(test_side_channel PRIVATE ceepew_portable)
target_include_directories(test_side_channel PRIVATE ${COMMON_INCLUDES})
add_test(NAME test_side_channel COMMAND test_side_channel)

# test_ascon — tests crypto_ascon_aead encrypt/decrypt
add_executable(test_ascon
    ${CMAKE_CURRENT_SOURCE_DIR}/test_ascon.c
)
target_link_libraries(test_ascon PRIVATE ceepew_portable)
target_include_directories(test_ascon PRIVATE ${COMMON_INCLUDES})
add_test(NAME test_ascon COMMAND test_ascon)

# test_hamming — tests ecc_hamming encode/decode round-trip
add_executable(test_hamming
    ${CMAKE_CURRENT_SOURCE_DIR}/test_hamming.c
)
target_link_libraries(test_hamming PRIVATE ceepew_portable)
target_include_directories(test_hamming PRIVATE ${COMMON_INCLUDES})
add_test(NAME test_hamming COMMAND test_hamming)

# test_sha256 — tests crypto_sha256_compute
add_executable(test_sha256
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sha256.c
)
target_link_libraries(test_sha256 PRIVATE ceepew_mbedtls ceepew_portable)
target_link_libraries(test_sha256 PRIVATE ${MBEDCRYPTO_STATIC_LIBS})
target_include_directories(test_sha256 PRIVATE ${COMMON_INCLUDES})
add_test(NAME test_sha256 COMMAND test_sha256)

# test_hkdf — tests crypto_hkdf_derive
add_executable(test_hkdf
    ${CMAKE_CURRENT_SOURCE_DIR}/test_hkdf.c
)
target_link_libraries(test_hkdf PRIVATE ceepew_mbedtls ceepew_portable)
target_link_libraries(test_hkdf PRIVATE ${MBEDCRYPTO_STATIC_LIBS})
target_include_directories(test_hkdf PRIVATE ${COMMON_INCLUDES})
add_test(NAME test_hkdf COMMAND test_hkdf)

# test_eddsa — Ed25519 EdDSA sign/verify round-trip
add_executable(test_eddsa
    ${CMAKE_CURRENT_SOURCE_DIR}/test_eddsa.c
)
target_link_libraries(test_eddsa PRIVATE ceepew_mbedtls ceepew_portable)
target_link_libraries(test_eddsa PRIVATE ${MBEDCRYPTO_STATIC_LIBS})
target_include_directories(test_eddsa PRIVATE ${COMMON_INCLUDES})
add_test(NAME test_eddsa COMMAND test_eddsa)

# test_huffman — static Huffman compress/decompress round-trip
add_executable(test_huffman
    ${CMAKE_CURRENT_SOURCE_DIR}/test_huffman.c
)
target_link_libraries(test_huffman PRIVATE ceepew_portable)
target_include_directories(test_huffman PRIVATE ${COMMON_INCLUDES})
add_test(NAME test_huffman COMMAND test_huffman)

# test_session_pairing — dual-session pairing, HKDF key convergence & AEAD messaging round-trip
add_executable(test_session_pairing
    ${CMAKE_CURRENT_SOURCE_DIR}/test_session_pairing.c
)
target_link_libraries(test_session_pairing PRIVATE ceepew_mbedtls ceepew_portable)
target_link_libraries(test_session_pairing PRIVATE ${MBEDCRYPTO_STATIC_LIBS})
target_include_directories(test_session_pairing PRIVATE ${COMMON_INCLUDES})
add_test(NAME test_session_pairing COMMAND test_session_pairing)

# ── ARQ library: compiles the REAL ecc_arq.c with host runtime stubs ──
set(ARQ_SRCS
    ${COMPONENT_DIR}/components/ecc/ecc_arq.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/arq_runtime_stub.c
)
add_library(ceepew_arq OBJECT ${ARQ_SRCS})
target_include_directories(ceepew_arq PRIVATE ${COMMON_INCLUDES})

# test_arq_sync — ARQ post-derive sync-exchange regression (floor-guard fix)
add_executable(test_arq_sync
    ${CMAKE_CURRENT_SOURCE_DIR}/test_arq_sync.c
)
target_link_libraries(test_arq_sync PRIVATE ceepew_portable ceepew_arq)
target_include_directories(test_arq_sync PRIVATE ${COMMON_INCLUDES})
add_test(NAME test_arq_sync COMMAND test_arq_sync)

# test_arq_fault_injection — ARQ transport fault injection under packet drop/corruption
add_executable(test_arq_fault_injection
    ${CMAKE_CURRENT_SOURCE_DIR}/test_arq_fault_injection.c
)
target_link_libraries(test_arq_fault_injection PRIVATE ceepew_portable ceepew_arq)
target_include_directories(test_arq_fault_injection PRIVATE ${COMMON_INCLUDES})
add_test(NAME test_arq_fault_injection COMMAND test_arq_fault_injection)




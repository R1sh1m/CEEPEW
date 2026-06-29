# tests/CMakeLists.txt
#
# Internal-use test code. NOT built into production firmware unless
# CONFIG_CEEPEW_DEVELOPMENT_MODE is enabled in menuconfig. When disabled this
# component registers with no source files and contributes nothing
# (apart from trivial component metadata) to the final binary.
#
# To run the tests after enabling the option, the firmware auto-invokes
# integration_tests_run_all() at the end of app_main. See tests/README.md
# for the human-and-AI-agent workflow.

set(test_srcs "")
if(CONFIG_CEEPEW_DEVELOPMENT_MODE)
    list(APPEND test_srcs
        "main/integration_test_e2e.c"
        "main/integration_test_pairing_ui.c"
        "main/integration_test_pairing_e2e.c"
        "main/test_arq_backoff.c"
        "main/test_hop.c"
        "main/test_pipeline_memory.c"
        "main/test_power.c"
        "main/test_replay.c"
        "main/test_replay_comprehensive.c"
        "main/test_transport_hop.c"
        "main/test_pairing_handoff.c"
        "main/test_pairing_convergence.c"
        "main/test_rng_health.c"
        "components/crypto/curve25519_test.c"
        "components/ceepew_hal/ui_manager_test.c"
        "components/ceepew_hal/ui_cryptogram_test.c"
        "components/ceepew_hal/hal_temp_test.c"
        # "components/ceepew_hal/hal_oled_test.c"  # deferred: hal_oled facade not yet implemented
    )
endif()

idf_component_register(
    SRCS "${test_srcs}"
    INCLUDE_DIRS
        "main"
        "components/ceepew_hal"
        "components/crypto"
        "."
    REQUIRES
        main
        ceepew_hal
        ceepew_oled
        crypto
        ecc
        compress
        transport
        mem
        esp_system
        esp_timer
        freertos
        log
)

# Single chokepoint for the runtime-selftest macro. Test files that
# gate their bodies with #ifdef CEEPEW_ENABLE_SELFTEST see this define
# when the test suite is being built.
if(CONFIG_CEEPEW_DEVELOPMENT_MODE AND NOT CMAKE_SCRIPT_MODE_FILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CEEPEW_ENABLE_SELFTEST)
endif()

# Triage / build-time check to prevent accidental production builds without device-binding
if(CONFIG_SECURE_BOOT_V2_ENABLED AND CONFIG_SECURE_FLASH_ENCRYPTION_ENABLED)
    if(NOT CONFIG_CEEPEW_EFUSE_HMAC_KEY)
        message(FATAL_ERROR "PRODUCTION BUILD ERROR: CONFIG_SECURE_BOOT_V2_ENABLED and CONFIG_SECURE_FLASH_ENCRYPTION_ENABLED are enabled (production mode), but CONFIG_CEEPEW_EFUSE_HMAC_KEY is not set. A production build must enable device-bound key derivation via eFuse!")
    endif()
endif()

idf_component_register(
    SRCS
        "main.c"
        "session_fsm.c"
        "session_msgstore.c"
        "session_send.c"
        "task_ui.c"
        "task_session.c"

    INCLUDE_DIRS
        "."
    REQUIRES
        ceepew_common
        ceepew_hal
        ceepew_oled
        crypto
        ecc
        compress
        transport
        mem
        tools
        tests
        esp_system
        esp_wifi
        esp_coex
        esp_event
        esp_timer
        freertos
        log
        bt
)

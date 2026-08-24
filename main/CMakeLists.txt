# eFuse device-bound key derivation was intentionally dropped by design decision
# (see WORK_SESSION_LOG / git history). Production builds no longer require
# CONFIG_CEEPEW_EFUSE_HMAC_KEY; secure boot + flash encryption stand on their own.

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

target_link_libraries(${COMPONENT_LIB} PUBLIC ceepew_hal transport ceepew_oled crypto ecc compress mem tools)


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
    # NOTE: tests/ excluded from firmware REQUIRES (no link dependency:
    # test entry points in main.c are behind #if 0; host suite builds via
    # tests/host + ceepew_pipeline.py --mode host, unaffected by this list).
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
        esp_system
        esp_wifi
        esp_coex
        esp_event
        esp_timer
        freertos
        log
        bt
)


idf_component_register(
    SRCS
        "sniffer_main.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        sniffer
        esp_wifi
        esp_event
        esp_timer
        freertos
        driver
        esp_driver_gpio
        esp_driver_uart
        log
)
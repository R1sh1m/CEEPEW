idf_component_register(SRCS "sniffer.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event esp_timer freertos driver nvs_flash
                       PRIV_REQUIRES esp_phy esp_driver_uart)
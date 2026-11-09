idf_component_register(SRCS "sniffer.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event esp_timer freertos driver nvs_flash
                       PRIV_REQUIRES esp_phy esp_driver_uart)

# Example app mains (reference only — not auto-registered to avoid duplicate app_main):
#   sniffer_app.c      — full-featured sniffer app (button stop, UART hex output)
#   sniffer_minimal.c  — minimal promiscuous capture app
# Copy the desired example into your project's main/ to build a sniffer firmware.
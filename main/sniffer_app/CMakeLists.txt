idf_component_register(SRCS "sniffer_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES sniffer
                       PRIV_REQUIRES esp_wifi)
/*
 * components/sniffer/sniffer.h
 *
 * ESP32 Promiscuous Wi-Fi Sniffer for CEE-PEW OTA Capture
 * Outputs IEEE 802.11 Action frames (ESP-NOW, OUI 18:FE:34) as hex lines over UART.
 * Format: <timestamp_us> <len> <hex_payload>
 */

#ifndef SNIFFER_H
#define SNIFFER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sniffer configuration */
typedef struct {
    uint8_t channel;              /* Wi-Fi channel to sniff (1-13) */
    bool    filter_espnow_only;   /* Only output ESP-NOW frames (OUI 18:FE:34) */
    int     uart_port;            /* UART port for output (default UART_NUM_0) */
    int     baud_rate;            /* UART baud rate (default 921600) */
} sniffer_config_t;

/* Default configuration */
static inline sniffer_config_t sniffer_default_config(void)
{
    sniffer_config_t cfg = {
        .channel = 1,
        .filter_espnow_only = true,
        .uart_port = 1,  /* Use UART1 to avoid conflict with system logs on UART0 */
        .baud_rate = 921600
    };
    return cfg;
}

#define SNIFFER_DEFAULT_CONFIG() sniffer_default_config()

/* Initialize and start the sniffer */
esp_err_t sniffer_init(const sniffer_config_t *config);

/* Stop the sniffer */
esp_err_t sniffer_stop(void);

/* Check if sniffer is running */
bool sniffer_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* SNIFFER_H */
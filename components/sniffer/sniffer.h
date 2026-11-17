/*
 * components/sniffer/sniffer.h
 *
 * ESP32 Promiscuous Wi-Fi Sniffer for CEE-PEW OTA Capture
 * Outputs IEEE 802.11 Action frames (ESP-NOW, OUI 18:FE:34) as hex lines over
 * UART. Format: <timestamp_us> <len> <hex_payload>
 */

#ifndef SNIFFER_H
#define SNIFFER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sniffer configuration */
typedef struct {
  uint8_t channel;         /* Wi-Fi channel to sniff (1-13) */
  bool filter_espnow_only; /* Only output ESP-NOW frames (OUI 18:FE:34) */
  int uart_port;           /* UART port for frame output (default UART_NUM_1) */
  int cmd_uart_port; /* UART port for command input (default UART_NUM_2) */
  int baud_rate;     /* UART baud rate (default 921600) */
} sniffer_config_t;

/* Default configuration */
static inline sniffer_config_t sniffer_default_config(void) {
  sniffer_config_t cfg = {.channel = 1,
                          .filter_espnow_only = true,
                          .uart_port = 0,     /* UART0 (USB / console) */
                          .cmd_uart_port = 0, /* UART0 (USB / console) */
                          .baud_rate = 921600};
  return cfg;
}

#define SNIFFER_DEFAULT_CONFIG() sniffer_default_config()

/* Initialize and start the sniffer */
esp_err_t sniffer_init(const sniffer_config_t *config);

/* Stop the sniffer */
esp_err_t sniffer_stop(void);

/* Check if sniffer is running */
bool sniffer_is_running(void);

/* Sniffer app entry point */
void sniffer_app_main(void);

#ifdef __cplusplus
}
#endif

#endif /* SNIFFER_H */
/* Stub esp_now.h for host compilation.
 * Provides just enough of the ESP-NOW API surface for the CEE-PEW
 * portable headers (hal_radio.h) to compile outside ESP-IDF.
 */

#ifndef ESP_NOW_H
#define ESP_NOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_NOW_MAX_DATA_LEN 250U

typedef enum {
    ESP_NOW_SEND_SUCCESS = 0,
    ESP_NOW_SEND_FAIL = 1,
} esp_now_send_status_t;

typedef void (*esp_now_recv_cb_t)(const uint8_t *mac_addr, const uint8_t *data, int data_len);

#ifdef __cplusplus
}
#endif

#endif /* ESP_NOW_H */

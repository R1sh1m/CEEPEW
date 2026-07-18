/* Stub esp_log.h for host compilation.
 * Replaces ESP-IDF logging macros with no-ops so that portable
 * component source files can compile outside the ESP-IDF build system.
 */

#ifndef ESP_LOG_H
#define ESP_LOG_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR = 1,
    ESP_LOG_WARN = 2,
    ESP_LOG_INFO = 3,
    ESP_LOG_DEBUG = 4,
    ESP_LOG_VERBOSE = 5
} esp_log_level_t;

#define ESP_LOGE(tag, fmt, ...) do { (void)tag; fprintf(stderr, "E/" tag ": " fmt "\n", ##__VA_ARGS__); } while(0)
#define ESP_LOGW(tag, fmt, ...) do { (void)tag; fprintf(stdout, "W/" tag ": " fmt "\n", ##__VA_ARGS__); } while(0)
#define ESP_LOGI(tag, fmt, ...) do { (void)tag; fprintf(stdout, "I/" tag ": " fmt "\n", ##__VA_ARGS__); } while(0)
#define ESP_LOGD(tag, fmt, ...) do { (void)tag; fprintf(stdout, "D/" tag ": " fmt "\n", ##__VA_ARGS__); } while(0)
#define ESP_LOGV(tag, fmt, ...) do { (void)tag; fprintf(stdout, "V/" tag ": " fmt "\n", ##__VA_ARGS__); } while(0)

#ifdef __cplusplus
}
#endif

#endif /* ESP_LOG_H */

/* Stub esp_wifi_types.h for host compilation.
 * hal_radio.h references wifi_ps_type_t in hal_radio_set_power_save().
 * Defined as a plain int type rather than an enum because the host test
 * CMakeLists.txt globally defines WIFI_PS_NONE=0 and WIFI_PS_MIN_MODEM=0
 * as macros, which would conflict with enum constants of the same name.
 */

#ifndef ESP_WIFI_TYPES_H
#define ESP_WIFI_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int wifi_ps_type_t;

#ifdef __cplusplus
}
#endif

#endif /* ESP_WIFI_TYPES_H */

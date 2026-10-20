/* components/transport/transport_capture.h
 *
 * On-node promiscuous OTA capture (opt-in Phase-3 debug aid).
 *
 * When CONFIG_CEEPEW_CAPTURE is enabled, the session task starts this
 * sniffer AFTER BLE teardown when entering ESP-NOW Phase 3. It installs a
 * WiFi promiscuous-mode callback (MGMT filter) that copies ESP-NOW vendor
 * action frames (OUI 18:fe:34) into a static ring, plus a low-priority
 * logger task that prints ts/rssi/ch/len/hex per entry.
 *
 * Guarded behind CONFIG_CEEPEW_CAPTURE (default n) so normal builds are
 * unaffected: with the flag off the start/stop calls compile to no-ops.
 */

#ifndef TRANSPORT_CAPTURE_H
#define TRANSPORT_CAPTURE_H

#include <stdint.h>
#include "ceepew_assert.h"

/* sdkconfig.h MUST precede the CONFIG_CEEPEW_CAPTURE guard: this header is
 * self-contained and cannot rely on the includer having pulled sdkconfig.h
 * in transitively (without it the guard is always false and callers
 * silently get the no-op stubs even in capture builds). */
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ring depth and per-entry payload cap. 4 x ~308 B ~= 1.2 KB static. */
#define TRANSPORT_CAPTURE_RING_LEN  4U
#define TRANSPORT_CAPTURE_MAX_BYTES 300U

/* Install MGMT promiscuous filter + RX callback, enable promiscuous mode,
 * and start the logger task. Must be called AFTER BLE teardown when WiFi
 * is up (esp_wifi_stop() clears promiscuous state). Idempotent. */
CeePewErr_t transport_capture_start(void);

/* Disable promiscuous mode and stop the logger task. */
void transport_capture_stop(void);

/* True once transport_capture_start() has succeeded. */
bool transport_capture_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_CAPTURE_H */

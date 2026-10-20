# OTA capture scaffold — 3rd-ESP32 promiscuous sniffer (docs only)

> Status: **plan / scaffold**. No firmware in this directory. Any future
> sniffer firmware lives under a new `components/` or standalone app and is
> **not** part of this change. This document records the agreed capture plan
> so the workflow manager can schedule the firmware + bench work.

## 1. Goal

Produce over-the-air (OTA) packet captures of the two CEE-PEW nodes that let
us recompute **Fig 3b / 3c** (delivery / timing distributions) from sniffer
timestamps instead of in-band serial log stamps, and support the CCMP-injection
/ replay-vector security checks claimed in the paper.

Non-goals: modifying `main/` firmware, changing the paper `.tex` sources,
stripping PDF metadata (separate lane).

## 2. Sniffer node plan (3rd ESP32)

- Dedicated capture board (any spare ESP32; same silicon family preferred so
  radiotap/PHY behaviour matches the DUTs).
- Wi-Fi init in **`WIFI_MODE_NULL`** (no STA/AP association, no TX from the
  sniffer itself):
  ```c
  ESP_ERROR_CHECK(esp_netif_init());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_channel(CH, WIFI_SECOND_CHAN_NONE));
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));  // action frames
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&sniffer_cb));
  ```
- Promiscuous filter: enable **management + data + action-frame** subtypes
  (`WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA`), then
  software-filter to ESP-NOW action frames (category/vendor OUI) inside
  `sniffer_cb` to keep the SD path from saturating.
- `sniffer_cb` must be ISR-light: copy `wifi_promiscuous_pkt_t` header +
  payload into a RAM ring, defer SD writes to a low-priority task.
- Storage option A (preferred): **SD card (SPI/SDMMC) pcap** — write a global
  pcap header once, then per-packet `pcaprec_hdr_t` (ts_sec/ts_usec/incl_len/
  orig_len) + raw 802.11 bytes. Pre-allocate the file / use 512-byte aligned
  multi-block writes; stop capture on card-full, never block the RX path.
- Storage option B (fallback): **apptrace / UART streaming** to host while the
  two DUTs run on their own USB ports. Lower fidelity (host-side jitter, drop
  risk) — acceptable for bring-up only, not for Fig 3b/3c numbers.
- Channel policy: fix the DUTs and sniffer to one channel (default 1 or 6);
  record the channel in the capture filename (`ch1_...pcap`). No hopping
  during a timed run.

## 3. tshark fields for Fig 3b/3c recompute

Required per-packet columns (sniffer-side ground truth):

| Field | Why |
|-------|-----|
| `frame.time_epoch` / `frame.time` | sniffer arrival timestamp (replaces serial stamps) |
| `wlan.sa` / `wlan.da` / `wlan.ta` / `wlan.ra` | attribute each frame to node A / node B |
| `wlan.seq` | sequence-number gaps = loss / retry accounting |
| `data.len` | payload-length histogram (Fig 3b input) |
| `radiotap.dbm_antsignal` | RSSI distribution (Fig 3c input) |
| `wlan.fc.type` / `wlan.fc.subtype` | isolate ESP-NOW action frames from beacons/probes |

Example extraction (adjust `wlan` display-filter to the DUT OUIs):

```powershell
# Tab-separated per-packet table for the Fig 3b/3c notebook script
tshark -r ota_run1.pcap -T fields -e frame.time_epoch -e wlan.sa -e wlan.da `
  -e wlan.seq -e data.len -e radiotap.dbm_antsignal `
  -Y "wlan.fc.type==0 || wlan.fc.type==2" -E header=y -E separator=, > fig3_recompute.csv

# Quick RSSI sanity histogram
tshark -r ota_run1.pcap -T fields -e radiotap.dbm_antsignal `
  -Y "wlan.sa==AA:BB:CC:DD:EE:FF" | Sort-Object | Get-Unique -AsString
```

Recompute rule: inter-arrival deltas come from `frame.time_epoch`
differences on the **sniffer clock**; serial-log timestamps are kept only as
a cross-check column, never as the primary axis.

## 4. GPIO / esp_timer timing method (replaces serial stamps)

Serial-print timestamps are polluted by UART blocking + log-level gating, so
timed runs use hardware timestamps:

- Each DUT toggles a spare **GPIO on TX-complete and on RX-callback entry**
  (single `gpio_set_level` — ~100 ns scale, no printf in the ISR path).
- Sniffer (or a logic analyser / second GPIO capture) records edges; the
  authoritative delta is `esp_timer_get_time()` (µs, 64-bit) sampled at the
  same points and embedded in the frame metadata / sidecar log.
- Procedure: GPIO edge → `t_tx_done` / `t_rx_cb`; OTA delta =
  `t_rx_cb - t_tx_done` per sequence number joined on `wlan.seq`.
- Report mean / p50 / p99 over ≥ 1000 frames per configuration; state ESP-IDF
  version, CPU frequency, and channel in every results file.

## 5. CCMP-injection + replay-vector checklist

To be executed against the OTA captures before the paper's security claims
are treated as verified:

- [ ] **Fresh-ciphertext acceptance**: capture N legitimate CCMP frames, confirm
      the peer accepts each exactly once (join on `wlan.seq` + CCMP packet number).
- [ ] **Bit-flip rejection**: flip one byte in a captured ciphertext, re-inject
      over the air, confirm MIC failure / drop (no RX-callback accept, no ACK path).
- [ ] **Replay rejection**: re-inject an unmodified prior frame (same CCMP
      packet number / nonce), confirm drop via the replay window; check the
      serial sidecar shows no duplicate delivery to the application layer.
- [ ] **Cross-session replay**: replay a frame from session *k* into session
      *k+1* (after rekey), confirm rejection (wrong session key / nonce domain).
- [ ] **Monotonicity audit**: plot accepted CCMP packet numbers vs wall time —
      must be strictly increasing per peer; any gap/retreat is a finding.
- [ ] **Tooling note**: injection uses raw 802.11 TX from a test ESP32
      (`esp_wifi_80211_tx`) on the same channel; keep injection runs in
      separate `.pcap` files from the Fig 3b/3c measurement runs.

## 6. Bench + file conventions (for the implementing lane)

- Bench: 2× DUTs at fixed distance/orientation + 1× sniffer, all on the same
  channel; photo + distance log per run; no microwave / BLE-interference
  sources nearby (or note them).
- Naming: `tools/ota_capture/captures/<date>_ch<CH>_<config>_{dut,sniff}.pcap`
  plus a one-line `.txt` sidecar (commit hash, IDF version, channel, distance).
- Validation (workflow manager): `tshark -r <file> -T fields ...` smoke-reads
  the columns in §3; Fig 3b/3c recompute script consumes `fig3_recompute.csv`.

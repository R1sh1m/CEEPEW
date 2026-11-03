# X4 - Header tamper (OTA)

## Purpose
Verify that CEE-PEW rejects header-tampered frames over the air. The ESL header is part of the Ascon associated data, so any tampering with seq/timestamp_s/flags/nonce_counter fails tag verification.

## Build
- build_bench3 (Full profile, FEC off, bench flags on)
- ESP-IDF v6.0.3, 160 MHz, PM disabled
- Boards: COM3 (Device A), COM4 (Device B)

## Procedure
1. Flash build_bench3 to both boards
2. Start monitor: python tools/ceepew_monitor.py --port COM3 --port COM4 --duration 300 --log data/raw/X4_header_tamper_20261029/capture_dual.log
3. Pair devices (PIN confirmation on both)
4. After pairing, use raw 802.11 TX from a third ESP32 to inject header-tampered frames
5. Observe peer rejection at ESL layer (err=3 or err=5)

## Expected outcome
- All header-tampered frames rejected at ESL/AEAD layer
- No tampered frame reaches application layer

## Analysis
- Count rejection kinds: err=3 (ESL header/MAC), err=5 (crypto box), err=17 (replay commit)
- Report rejection rate with Wilson CI

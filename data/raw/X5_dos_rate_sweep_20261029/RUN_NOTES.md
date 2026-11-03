# X5 - DoS rate sweep (OTA)

## Purpose
Measure receiver saturation under forged-frame DoS attack. Forged frames with fresh sequence numbers pass the DoS gate and reach AEAD rejection.

## Build
- build_bench3 (Full profile, FEC off, bench flags on)
- ESP-IDF v6.0.3, 160 MHz, PM disabled
- Boards: COM3 (Device A), COM4 (Device B)

## Procedure
1. Flash build_bench3 to both boards
2. Start monitor on both ports
3. Pair devices
4. Inject forged frames at increasing rates (1, 10, 50, 100 frames/s)
5. Measure queue high-water, drops, valid-traffic survival

## Expected outcome
- Saturation rate identified
- Valid traffic survival rate measured
- Mitigation effectiveness quantified

## Analysis
- Parse capture_dual.log for queue depth, drops, valid deliveries
- Report saturation threshold and survival rate

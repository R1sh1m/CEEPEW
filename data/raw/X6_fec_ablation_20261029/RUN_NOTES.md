# X6 - FEC ablation (OTA)

## Purpose
Compare delivery and goodput with FEC on vs off. FEC corrected zero bits across previous campaign.

## Builds
- FEC OFF: build_bench3 (Full profile, FEC disabled)
- FEC ON:  build_fec2 (Full profile, FEC enabled)
- ESP-IDF v6.0.3, 160 MHz, PM disabled
- Boards: COM3 (Device A), COM4 (Device B)

## Procedure
### Arm 1: FEC OFF
1. Flash build_bench3 to both boards
2. Start monitor, pair devices, run BENCH58 campaign
3. Capture 480s

### Arm 2: FEC ON
1. Flash build_fec2 to both boards
2. Start monitor, pair devices, run BENCH58 campaign
3. Capture 480s

## Expected outcome
- Delivery rates for both arms
- Goodput comparison
- TOST equivalence test

## Analysis
- Run run_x8_delivery.py on both captures
- Compare delivery rates, frame counts, error rates

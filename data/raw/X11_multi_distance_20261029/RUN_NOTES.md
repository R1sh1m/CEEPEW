# X11 - Multi-distance and repeated sessions (OTA)

## Purpose
Measure delivery and latency versus distance for external validity.

## Build
- build_bench3 (Full profile, FEC off, bench flags on)
- ESP-IDF v6.0.3, 160 MHz, PM disabled

## Procedure
### Arm 1: 20 cm (baseline)
1. Flash build_bench3, position boards 20 cm apart
2. Pair, run 5 sessions, capture each

### Arm 2: 100 cm
1. Position boards 100 cm apart
2. Pair, run 5 sessions, capture each

### Arm 3: 300 cm
1. Position boards 300 cm apart
2. Pair, run 5 sessions, capture each

## Expected outcome
- Delivery vs distance curve
- Latency vs distance
- Per-session and pooled results

## Analysis
- Run run_x8_delivery.py on each session
- Aggregate per distance
- Report with clustering stated

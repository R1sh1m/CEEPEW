# X9 - Memory instrumentation (OTA)

## Purpose
Measure region pool high-water, stack HWM, queue HWM during operation.

## Build
- build_bench3 (Full profile, FEC off, bench flags on)
- ESP-IDF v6.0.3, 160 MHz, PM disabled

## Procedure
1. Flash build_bench3 to both boards
2. Start monitor on both ports
3. Pair devices, run BENCH58 campaign
4. Monitor memory usage via serial logs
5. Capture 480s

## Expected outcome
- Region pool high-water mark
- Stack high-water marks per task
- Queue high-water marks

## Analysis
- Parse capture logs for memory-related log lines
- Report high-water marks with evidence

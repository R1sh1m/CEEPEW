# E5 Sync-Pulse Latency Report: E5_sync_pulse_20261018

- Clock: device timestamps (device boot-time microseconds)

## Parsing Summary

- Total lines scanned: 40555
- Records recovered: 300
- Lines skipped: 40255

## TX Intervals (Same-Clock)

### DEVICE_A (TX)

| Interval | N | Min (µs) | Median (µs) | P95 (µs) | Max (µs) | Mean (µs) |
|----------|---|----------|-------------|----------|----------|-----------|
| submit->pulse | 80 | 346658 | 365524.0 | 383712.35 | 395325 | 365551.8 |
| pulse->sendcb | 80 | 4060 | 4363.5 | 7839.0 | 11305 | 4940.9 |

### DEVICE_B (TX)

| Interval | N | Min (µs) | Median (µs) | P95 (µs) | Max (µs) | Mean (µs) |
|----------|---|----------|-------------|----------|----------|-----------|
| submit->pulse | 80 | 347076 | 370274.0 | 383367.64999999997 | 389087 | 370091.2 |
| pulse->sendcb | 80 | 4091 | 4365.5 | 7724.75 | 9827 | 4904.6 |

## RX Intervals (Same-Clock)

### DEVICE_A (RX)

| Interval | N | Min (µs) | Median (µs) | P95 (µs) | Max (µs) | Mean (µs) |
|----------|---|----------|-------------|----------|----------|-----------|
| **edge->rxcb** | 71 | 6696 | 26620.0 | 52344.5 | 55680 | 28525.9 |
| rxcb->accept | 71 | 696772 | 727284.0 | 745110.5 | 760290 | 726629.7 |

### DEVICE_B (RX)

| Interval | N | Min (µs) | Median (µs) | P95 (µs) | Max (µs) | Mean (µs) |
|----------|---|----------|-------------|----------|----------|-----------|
| **edge->rxcb** | 68 | 6774 | 27214.0 | 52777.19999999999 | 341336571 | 5046872.0 |
| rxcb->accept | 68 | 696771 | 725585.5 | 745322.2 | 265880270 | 4624930.9 |

## Sequence Coverage

| Device | Role | Seq Range | Unique | Gaps | Duplicates |
|--------|------|-----------|--------|------|------------|
| DEVICE_A | TX | 0..79 | 80 | 0 | [] |
| DEVICE_A | RX | 69..69 | 1 | 0 | [69] |
| DEVICE_B | TX | 0..79 | 80 | 0 | [] |
| DEVICE_B | RX | 69..69 | 1 | 0 | [69] |

## Lost Sync Pulses (edge=0)

*(none)*

## Interpretation

**CRITICAL:** The two boards have **INDEPENDENT microsecond clocks** (esp_timer_get_time() counts from each board's own boot; they are NOT synchronised). Therefore we **NEVER** compute a difference between timestamps originating on different devices. Every reported statistic is a **same-clock interval**:

- TX-side (single device): `submit→pulse` and `pulse→sendcb`
- RX-side (single device): `edge→rxcb` (**headline one-way latency**) and `rxcb→accept`

The `edge→rxcb` interval captures radio TX + propagation + RX + callback delivery, all measured on the receiver's own clock.


# E2 Forge Reject Report: E2b_forged_rejection_20261018

- Window: 2.0 s
- Clock: device timestamps

## Forged TX Events

- Total forged TX: 16
- Forge armed count: 16

| Device | Count | Seqs |
|--------|-------|------|
| DEVICE_A | 8 | [9, 19, 29, 39, 49, 59, 69, 79] |
| DEVICE_B | 8 | [9, 19, 29, 39, 49, 59, 69, 79] |

## Crypto Rejections

- Total: 36
- By kind: {'replay': 2, 'ascon': 1, 'pipeline': 17, 'box': 16}

| Device | box | ascon | pipeline | replay | other |
|--------|-----|-------|----------|--------|-------|
| DEVICE_A | 8 | 1 | 9 | 1 | 0 |
| DEVICE_B | 8 | 0 | 8 | 1 | 0 |

## Matching Results

- **MATCHED 16 / 16**
- Median Δt: +400.00 ms
- Max |Δt|: +1500.00 ms

### Match Table

| Forged Seq | Forged Dev | Reject Kind | Reject Dev | Δt (us) | Δt (host) |
|------------|------------|-------------|------------|---------|-----------|
| 9 | DEVICE_A | box | DEVICE_B | +500.00 ms | +0.500s |
| 9 | DEVICE_B | box | DEVICE_A | +400.00 ms | +0.400s |
| 19 | DEVICE_A | box | DEVICE_B | +400.00 ms | +0.400s |
| 19 | DEVICE_B | box | DEVICE_A | +400.00 ms | +0.400s |
| 29 | DEVICE_A | box | DEVICE_B | +400.00 ms | +0.400s |
| 29 | DEVICE_B | box | DEVICE_A | +400.00 ms | +0.400s |
| 39 | DEVICE_A | box | DEVICE_B | +400.00 ms | +0.400s |
| 39 | DEVICE_B | box | DEVICE_A | +400.00 ms | +0.400s |
| 49 | DEVICE_A | box | DEVICE_B | +400.00 ms | +0.400s |
| 49 | DEVICE_B | box | DEVICE_A | +500.00 ms | +0.500s |
| 59 | DEVICE_A | box | DEVICE_B | +400.00 ms | +0.400s |
| 59 | DEVICE_B | box | DEVICE_A | +400.00 ms | +0.400s |
| 69 | DEVICE_A | box | DEVICE_B | +500.00 ms | +0.500s |
| 69 | DEVICE_B | box | DEVICE_A | +700.00 ms | +0.700s |
| 79 | DEVICE_A | box | DEVICE_B | +100.00 ms | +0.100s |
| 79 | DEVICE_B | box | DEVICE_A | +1500.00 ms | +1.500s |

## Unexplained Rejections

- Count: 6
- By kind: {'replay': 2, 'ascon': 1, 'pipeline': 3}

## Interpretation

This report uses **one-to-one time-correlated matching** to verify forgery rejection.

**Why time-correlation matters:** The bench forge path is one-shot: on the next transmit
after arming, exactly one outer-ciphertext byte is corrupted. ARQ retransmits that
corrupted frame, and it is dropped at the crypto stage. A clean retry of the same app
seq is then sent and accepted.

Therefore **app-level seq accounting** (`[BENCHRX] commit seq=N err=0`) **CANNOT** be
used to test forgery rejection — every forged seq will eventually appear committed
via its clean retry.

**Result:** All 16 forged frames were rejected by the peer's crypto authenticator.


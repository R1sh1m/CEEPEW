# E3 Replay Reject Report: E3_replay_20261019

- Window: 2.0 s
- Clock: device timestamps

## Replay Injection Events

- Total replay injections: 16
- Replay armed count: 16

| Device | Count | Payload Lens |
|--------|-------|--------------|
| DEVICE_A | 8 | [237, 237, 237, 233, 233, 233, 233, 233] |
| DEVICE_B | 8 | [237, 237, 237, 233, 233, 233, 233, 233] |

## Rejection Events (All Categories)

- Total: 52
- err=17 (CEEPEW_ERR_REPLAY): 0
- By kind: {'esl': 18, 'ascon': 1, 'pipeline': 17, 'box': 16}

| Device | replay_commit | replay_seq_unavail | esl | box | ascon | pipeline | other | err=17 |
|--------|---------------|--------------------|-----|-----|-------|----------|-------|--------|
| DEVICE_A | 0 | 0 | 9 | 8 | 1 | 9 | 0 | 0 |
| DEVICE_B | 0 | 0 | 9 | 8 | 0 | 8 | 0 | 0 |

## Matching Results

- **MATCHED 16 / 16**
- Matched rejections by err code: err=6: 16
- Median Δt: +0.00 ms
- Max |Δt|: +100.00 ms

### Match Table

| Replay Len | Replay Dev | Reject Layer | Reject Dev | Reject Err | Δt (us) | Δt (host) |
|------------|------------|--------------|------------|------------|---------|-----------|
| 237 | DEVICE_A | unexpected layer (err=6) | DEVICE_B | 6 | +0.00 ms | +0.000s |
| 237 | DEVICE_B | unexpected layer (err=6) | DEVICE_A | 6 | +100.00 ms | +0.100s |
| 237 | DEVICE_A | unexpected layer (err=6) | DEVICE_B | 6 | +0.00 ms | +0.000s |
| 237 | DEVICE_B | unexpected layer (err=6) | DEVICE_A | 6 | +0.00 ms | +0.000s |
| 237 | DEVICE_A | unexpected layer (err=6) | DEVICE_B | 6 | +100.00 ms | +0.100s |
| 237 | DEVICE_B | unexpected layer (err=6) | DEVICE_A | 6 | +0.00 ms | +0.000s |
| 233 | DEVICE_A | unexpected layer (err=6) | DEVICE_B | 6 | +0.00 ms | +0.000s |
| 233 | DEVICE_B | unexpected layer (err=6) | DEVICE_A | 6 | +100.00 ms | +0.100s |
| 233 | DEVICE_A | unexpected layer (err=6) | DEVICE_B | 6 | +100.00 ms | +0.100s |
| 233 | DEVICE_B | unexpected layer (err=6) | DEVICE_A | 6 | +0.00 ms | +0.000s |
| 233 | DEVICE_A | unexpected layer (err=6) | DEVICE_B | 6 | +0.00 ms | +0.000s |
| 233 | DEVICE_B | unexpected layer (err=6) | DEVICE_A | 6 | +0.00 ms | +0.000s |
| 233 | DEVICE_A | unexpected layer (err=6) | DEVICE_B | 6 | +100.00 ms | +0.100s |
| 233 | DEVICE_B | unexpected layer (err=6) | DEVICE_A | 6 | +100.00 ms | +0.100s |
| 233 | DEVICE_A | unexpected layer (err=6) | DEVICE_B | 6 | +0.00 ms | +0.000s |
| 233 | DEVICE_B | unexpected layer (err=6) | DEVICE_A | 6 | +100.00 ms | +0.100s |

## Unexplained Rejections

- Count: 36
- By kind: {'esl': 2, 'ascon': 1, 'pipeline': 17, 'box': 16}
- By err: {3: 2, 5: 34}

## Interpretation

This report uses **one-to-one time-correlated matching** to verify replay rejection.

**Why time-correlation matters:** The E3 replay hook replays a stale ESL header with a
fresh ARQ seq downstream, so the replayed frame passes ARQ as a new frame and reaches
the replay window. ARQ retransmission means a dropped frame can be followed by a clean
retry of the same application sequence number, so per-seq accounting would report the
exact opposite of the truth.

Therefore **app-level seq accounting** CANNOT be used to test replay rejection — every
replayed seq will eventually appear committed via its clean retry.

**Result:** 16/16 replayed frames matched to peer rejections (16x unexpected err code).
- 16 matched to an unexpected err code — investigate (not a replay-window commit).


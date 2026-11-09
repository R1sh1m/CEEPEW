# E3 Replay Reject Report: X3_replay_reject_20261109

- Window: 2.0 s
- Clock: device timestamps

## Replay Injection Events

- Total replay injections: 2
- Replay armed count: 2

| Device | Count | Payload Lens |
|--------|-------|--------------|
| DEVICE_A | 1 | [181] |
| DEVICE_B | 1 | [181] |

## Rejection Events (All Categories)

- Total: 9
- err=6 (CEEPEW_ERR_TRANSPORT, ESL sliding-window replay check): 2
- err=17 (CEEPEW_ERR_REPLAY, commit-time recheck): 0
- By kind: {'esl': 5, 'ascon': 2, 'pipeline': 2}

| Device | replay_commit | replay_seq_unavail | esl | box | ascon | pipeline | other | err=6 | err=17 |
|--------|---------------|--------------------|-----|-----|-------|----------|-------|-------|-------|
| DEVICE_A | 0 | 0 | 3 | 0 | 2 | 2 | 0 | 1 | 0 |
| DEVICE_B | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 1 | 0 |

## Matching Results

- **MATCHED 2 / 2**
- Matched rejections by err code: err=6: 2
- Median Δt: +350.00 ms
- Max |Δt|: +600.00 ms

### Match Table

| Replay Len | Replay Dev | Reject Layer | Reject Dev | Reject Err | Δt (us) | Δt (host) |
|------------|------------|--------------|------------|------------|---------|-----------|
| 181 | DEVICE_A | ESL sliding-window replay check | DEVICE_B | 6 | +600.00 ms | +0.600s |
| 181 | DEVICE_B | ESL sliding-window replay check | DEVICE_A | 6 | +100.00 ms | +0.100s |

## Unexplained Rejections

- Count: 7
- By kind: {'esl': 3, 'ascon': 2, 'pipeline': 2}
- By err: {3: 3, 5: 4}

## Interpretation

This report uses **one-to-one time-correlated matching** to verify replay rejection.

**Why time-correlation matters:** The E3 replay hook replays a stale ESL header with a
fresh ARQ seq downstream, so the replayed frame passes ARQ as a new frame and reaches
the replay window. ARQ retransmission means a dropped frame can be followed by a clean
retry of the same application sequence number, so per-seq accounting would report the
exact opposite of the truth.

Therefore **app-level seq accounting** CANNOT be used to test replay rejection — every
replayed seq will eventually appear committed via its clean retry.

**Result:** 2/2 replayed frames matched to peer rejections (2x unexpected err code).
- 2 matched to an unexpected err code — investigate (not a replay-window commit).


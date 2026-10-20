# E3 Replay Reject Report: E2b_forged_rejection_20261018

- Window: 2.0 s
- Clock: device timestamps

## Replay Injection Events

- Total replay injections: 0
- Replay armed count: 0

| Device | Count | Payload Lens |
|--------|-------|--------------|

## Rejection Events (All Categories)

- Total: 36
- err=17 (CEEPEW_ERR_REPLAY): 0
- By kind: {'esl': 2, 'ascon': 1, 'pipeline': 17, 'box': 16}

| Device | replay_commit | replay_seq_unavail | esl | box | ascon | pipeline | other | err=17 |
|--------|---------------|--------------------|-----|-----|-------|----------|-------|--------|
| DEVICE_A | 0 | 0 | 1 | 8 | 1 | 9 | 0 | 0 |
| DEVICE_B | 0 | 0 | 1 | 8 | 0 | 8 | 0 | 0 |

## Matching Results

- **MATCHED 0 / 0**

### Match Table

| Replay Len | Replay Dev | Reject Kind | Reject Dev | Reject Err | Δt (us) | Δt (host) |
|------------|------------|-------------|------------|------------|---------|-----------|

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

**Result:** All 0 replayed frames were rejected as replays (err=17) by the peer.


# E3 Replay Reject Report: logs

- Window: 2.0 s
- Clock: device timestamps

## Replay Injection Events

- Total replay injections: 0
- Replay armed count: 0

| Device | Count | Payload Lens |
|--------|-------|--------------|

## Rejection Events (All Categories)

- Total: 0
- err=6 (CEEPEW_ERR_TRANSPORT, ESL sliding-window replay check): 0
- err=17 (CEEPEW_ERR_REPLAY, commit-time recheck): 0
- By kind: {}

| Device | replay_commit | replay_seq_unavail | esl | box | ascon | pipeline | other | err=6 | err=17 |
|--------|---------------|--------------------|-----|-----|-------|----------|-------|-------|-------|

## Matching Results

- **MATCHED 0 / 0**

### Match Table

| Replay Len | Replay Dev | Reject Layer | Reject Dev | Reject Err | Δt (us) | Δt (host) |
|------------|------------|--------------|------------|------------|---------|-----------|

## Unexplained Rejections

- Count: 0

## Interpretation

This report uses **one-to-one time-correlated matching** to verify replay rejection.

**Why time-correlation matters:** The E3 replay hook replays a stale ESL header with a
fresh ARQ seq downstream, so the replayed frame passes ARQ as a new frame and reaches
the replay window. ARQ retransmission means a dropped frame can be followed by a clean
retry of the same application sequence number, so per-seq accounting would report the
exact opposite of the truth.

Therefore **app-level seq accounting** CANNOT be used to test replay rejection — every
replayed seq will eventually appear committed via its clean retry.

**Result:** No replay injections found in log; nothing to verify.


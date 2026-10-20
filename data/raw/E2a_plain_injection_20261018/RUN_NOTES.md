# E2(a) — Mode-mismatch injection: plain ESP-NOW peer vs CEE-PEW peer — run notes

## What this run actually tests, and an honest deviation from the written plan

The E2(a) plan assumed we would inject **80 plain BENCH58 frames into a CEE-PEW
receiver** and count the rejections. **That criterion is unachievable by
construction and the real N is 0, not 80.** Reason: BENCH58 transmit is gated on
`session_is_active()` **and** `session_sync_barrier_cleared()` in
`main/task_session.c`. A mode-mismatched pair can never clear the sync barrier —
the Mode C receiver rejects the Mode A peer's plain_ESPNOW framing at the ESL
transport stage — so the sender never transmits a single campaign frame. The
gate **stops the experiment before the injection can occur**.

This was predicted before the run was launched and is recorded here rather than
papered over. The run is therefore reported for what it genuinely measures:
**the handshake-level mutual rejection of two incompatible peers.** No planned
number has been silently reinterpreted, and no exclusion rule was relaxed. The
`n_planned=80` field in the manifest refers to the *planned* injection count and
is **not** the N of this result.

## Topology actually used
- **COM3 = `build_cap_plain`** (Mode A). Flashed manually with `esptool` using
  the existing validated binary; `flash_COM3.log` records the write and the
  device's own "Hash of data verified" readback. SHA-256 of the flashed image is
  **`9495ee6c…`** — byte-identical to the E1-A binary, so the sender is exactly
  the Mode-A image that E1-A validated.
- **COM4 = `build_cap_ceepew`** (Mode C). Not reflashed (`--no-flash`): the board
  still carried the E1-C/E2(b) image, SHA-256 **`d2390308…`**.
- The manifest's single `app_binary_sha256` field records the **receiver**
  (Mode C) image; the sender's hash is recorded above and in the manifest notes.
- Port-map change: the manifest was written assuming COM5; COM5 no longer exists
  after a server restart, so COM4 was used for the receiver.
- Duration 300 s, 115200 baud, channel 1, hopping off, 20 cm, both boards on the
  same USB hub.

## RESULT — no session forms in either direction; 0 frames injected

| metric | value |
|---|---|
| BENCH58 frames injected | **0** (`[BENCH58_TX]` = 0, `[BENCH58_DONE]` = 0) |
| application-layer messages delivered | **0** (`Message received` = 0) |
| sessions reaching `chat_menu` | **0** |
| `[POST_DERIVE_SYNC]` completions | **0** |
| `Post-derive sync timed out` | **11** (DEVICE_A 6, DEVICE_B 5) |
| `PAIRING_FAILED` | 25 (DEVICE_A 13, DEVICE_B 12) |
| total `Discard:` reject events | **628** |
| FSM state transitions | 56 |

### Rejections, attributed per device (the direction matters)

| reject stage | DEVICE_A (plain, COM3) | DEVICE_B (CEE-PEW, COM4) |
|---|---|---|
| `ESL transport header/MAC/replay check failed` | 0 | **12** |
| `exact_len=… exceeds decoded FEC len=…` | **264** | 0 |
| `RX decrypt pipeline failed` | **352** | 0 |
| **total** | **616** | **12** |

Traffic direction explains the asymmetry: DEVICE_A received 367 radio frames and
logged 347 `CRC & FEC verified`, while DEVICE_B received only 20 radio frames.
The CEE-PEW board's frames are rejected outright by the plain peer (a Mode A
build bypasses the ESL/Ascon application layer and therefore misparses Mode C
frames at the FEC length-trailer and decrypt-pipeline stages), whereas the plain
peer's much smaller handshake traffic meets the CEE-PEW receiver's ESL check
head-on — **this is the intended E2(a) signal**: `ESL transport header/MAC/
replay check failed` is a CEE-PEW-only reject path and it fires on the plain
peer's frames.

The message text `exact_len=%u exceeds decoded FEC len=%u` originates at
`main/task_session.c:1838`.

## Why the direction is what it is (not a wiring mistake)
The Mode C board is the busy transmitter here (`ARQ_TX` 361, send-callback 740)
because it keeps retrying its handshake, while the plain board — which never gets
a session — transmits little (`ARQ_TX` 6, send-callback 45). The reject counts
follow the traffic, not the port labelling. Device labelling was checked against
the log rather than assumed: only the non-bypass (Mode C) build can emit the
`ESL transport header` reject string, and it appears solely on DEVICE_B, which
confirms DEVICE_B is the CEE-PEW build.

## What this result supports, and what it does not
- **Supports:** a plain ESP-NOW peer cannot inject into, or establish a session
  with, a CEE-PEW peer; the CEE-PEW ESL transport check rejects its frames; and
  the BENCH58 gate prevents any campaign traffic on a mode-mismatched pair.
- **Does not support:** any statement about *reject rate under sustained
  injection at scale* — N here is a handshake-scale N (628 reject events), not a
  data-frame campaign. That distinction must be preserved in the paper.

## Capture-integrity notes
- The ciphertext on this run is **not** part of the frame-length comparison
  (E2(a) uses application crypto from neither side coherently); do not feed this
  run's frames into E1's length-equality analysis.
- Same on-node ring-hex degradation applies (registry `CAP-RING-DEGRADE`); this
  result does not depend on ring hex — it is derived entirely from intact short
  metadata lines.
- Raw file sizes: `capture_dual.log` 2,476,438 B; `capture_DEVICE_A.log`
  1,040,339 B; `capture_DEVICE_B.log` 1,392,205 B.

## Provenance caveat (concurrent agent)
A second agent was active in this repository during this window (untracked
`components/sniffer/`, `main/sniffer_app/`, `sniffer_project/`). The E2(a) sender
was flashed from the pre-existing `build_cap_plain/ceepew.bin` (timestamp
23:14:08) whose SHA-256 matches E1-A's recorded binary, so the sender image is
unaffected. Note the E2(b)-style rebuild trap does not apply here: no rebuild
was triggered, so the recorded hashes are the images that actually ran.

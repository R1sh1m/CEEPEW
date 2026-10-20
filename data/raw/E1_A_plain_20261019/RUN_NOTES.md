# E1-A (plain) — run notes — 2026-10-19 retry

**Status: PRE-REGISTRATION written before the run.** Results are appended after the
capture, before sealing. Nothing in the rules/checks below was changed after seeing data.

## Why this run exists
The sealed `E1_A_plain_20261018` capture cannot support the Mode-A cleartext claim: its two
pcaps contain **zero** frames whose `wlan.addr` is a DUT MAC and zero hits for the BENCH58
`0x41` payload marker (`.agents/MODE_A_CLEARTEXT_AUDIT.md`;
`data/derived/wireshark_views/README.md:61-67`). Root cause diagnosed from the 20261018 logs:
the on-node capture ring is 4 slots deep, the accept-all promiscuous filter was flooded by
beacons/MISC frames, and the drain path printed one byte at a time over a 115200 UART — the
rare DUT action frames were evicted/dropped and ~30 % of printed rows were
interleaved-corrupted. This run re-tests the same claim with the capture instrument repaired.
The old failed run stays on disk; it is neither superseded nor edited.

## Topology (unchanged from 20261018)
- Both boards run `build_cap_plain` (Mode A, plain: ESL/Ascon application crypto bypassed on
  the session data path).
- Both boards transmit the BENCH58 campaign (80 frames/board, 5 s spacing, 58-byte
  'A'-filled payloads) AND capture the peer's frames on their on-node capture ring. A node
  cannot hear its own TX, so every frame is captured by the PEER board.
- Ports: COM3 (Device A `8C:94:DF:97:D0:C0`), COM4 (Device B `1C:C3:AB:C2:52:AC`).
- Hopping OFF, fixed channel 1, 20 cm rig (same bench as 20261018; not re-measured),
  normal office Wi-Fi environment.
- Console 115200 baud (fixed constraint; changing it is a separate, unapproved change).
  Mitigation for the known 115200 interleave risk: one atomic `fwrite` per ring row.

## Firmware changes vs the 20261018 E1-A image (disclosed in full; nothing else changed)
1. `components/transport/transport_capture.c` — **capture-instrument repair only**:
   (a) restored the DUT OUI guard (`payload[24]==0x7F`, `payload[25..27]==18:FE:34`, verified
   against real Mode A/B/C DUT frames), so non-DUT traffic cannot evict DUT frames from the
   4-slot ring; (b) one `fwrite` per row instead of per-byte prints. No protocol, timing,
   threshold or payload byte was changed.
2. `components/transport/transport_ble.c` — **lock-hold fix**: the advertising re-arm that
   used to run while holding `s_ble_ctx_lock` is now deferred (`s_adv_restart_pending`) and
   drained after unlock via `transport_ble_service_pending_adv_restart()` (wrapper + three
   supervisor sites). Same defect class as the E1-A panic; GAP calls are unbounded queue
   posts when the BTC queue is full (source-verified; `.agents/BLE_LOCK_AUDIT_FINDINGS.md`).
   No protocol timing/threshold change.

## Pre-registered exclusion rule (declared before any result)
**None.** No frame, ring row, pairing cycle, or session is excluded for any reason. The
capture-integrity gap (TX frames minus peer-captured ring rows) is computed and reported with
its cause. A failed pairing cycle is reported as an observation, not excluded (the 20261018
run's cycle-1 post-derive sync timeout is reported the same way).

## Pre-registered acceptance checks (claim gate, declared before the run)
The Mode-A cleartext claim is supported only if ALL of:
1. ≥1 pcap frame whose `wlan.addr` matches a DUT MAC
   (`8c:94:df:97:d0:c0` and/or `1c:c3:ab:c2:52:ac`);
2. ≥1 such frame carrying the BENCH58 payload marker (a run of `0x41` bytes) at the ESP-NOW
   payload offset;
3. this run's own `mode_report` verdict, reported verbatim (a NOT-VISIBLE result will be
   reported as a contradiction; the claim stays unsupported).
If (1) or (2) fails, the run is reported as an apparatus failure — not as a negative result
about the protocol.

## BLE lock-fix evidence (pre-registered)
Scan the dual log for (i) panic/reset/watchdog signatures (`Guru Meditation`, `abort()`,
`rst:0x`, `Task watchdog`, BLE ctx-lock timeouts); (ii) the new fingerprint
`adv re-arm deferred to post-unlock` (presence = the fixed path executed; absence + no panic
= the path was not reached). No threshold or timing was touched to make this pass.

## Toolchain / build provenance
- ESP-IDF **v6.0.2** (`C:\esp\v6.0.2\esp-idf`, `git describe --tags` = v6.0.2), compiler
  `xtensa-esp-elf 15.2.0_20251204` — the campaign toolchain used for E1/E2a/E2b/E5/E3. The
  repo's `tools/setup_esp_idf.ps1` currently exports v6.0.3 paths despite its "6.0.2"
  synopsis; v6.0.3 is deliberately NOT used (campaign consistency, and v6.0.3 cannot link
  this capture configuration — `dram0_0_seg` overflow, see E3 `PREREGISTRATION.md` §8).
- Build dir `build_cap_plain`; sdkconfig sha256
  `a9798a153120f836869585d7cbde8f5bc77f262a91ef8a901893d2a29c320490` — identical to the value
  recorded in the sealed 20261018 manifest (the configuration did not change). Binary and
  generated `sdkconfig.h` hashes are recorded in the manifest.
- The build directory was rebuilt from clean with the v6.0.2 toolchain (its previous cache
  was configured against a different python environment and had to be discarded); the
  sdkconfig was restored byte-for-byte from its backup (hash above).

## Commands (driver)
`python tools/e1_run_mode.py --run-id E1_A_plain_20261019 --build-dir build_cap_plain
--ports COM3,COM4 --baud 115200 --duration 480 --flash -v`

## Post-registration instrument changes (2026-10-19 ~22:25, written while the capture was
## running and BEFORE any log from this run was read)
The pre-registration above is unchanged. Four build/instrument facts are recorded here:

1. **Link-time DRAM failure and fix.** The first `build_cap_plain` link failed:
   `.dram0.bss` overflowed `dram0_0_seg` by 448 B. Attribution (`build_cap_plain/ceepew.map`
   vs the sealed `build_cap_ceepew/ceepew.map`) showed the only new `.bss` was the 768-byte
   static row buffer added by the atomic-print repair (+768 B; the BLE fix added 1 B; all
   other objects identical). Fix: the row buffer is now a local in the capture logger task
   (zero `.bss`), and that task's stack was raised 1536 -> 4096 bytes (heap-backed, capture
   build only). Capture semantics (OUI guard, one atomic write per row) are unchanged.
2. **Ring payload cap 300 -> 288 B.** `TRANSPORT_CAPTURE_MAX_BYTES` was reduced to free 48 B
   of DRAM (required by the BENCH_TIMING image, which shares the fixed source). Non-truncation
   argument: an ESP-NOW frame is bounded by 24 (MAC header) + 250 (ESP-NOW payload ceiling) +
   4 (FCS) = 278 B, and the longest genuine DUT frame in the 2026-10-19 OTA sniffer capture
   (`frame contains 7f:18:fe:34`) was 269 B. No real DUT frame can be truncated by a 288 B cap.
3. **sdkconfig text vs compiled configuration.** After the v6.0.2 reconfigure the *text*
   `sdkconfig` sha256 is `d6b54f9dc17da7eea471e718c8f55c23cc425a464c907ba18ed7813b7f54e4be`
   (the sealed 20261018 text hash was `a9798a15...`), but the generated `config/sdkconfig.h`
   is **byte-identical** to the sealed build:
   `9d902838b8ee4c0e8d34ccdfed42cac7aa30e754bb9f88dbfd3d948b672e2ec6`. The effective
   compiled configuration is therefore identical; only the generated text file differs.
4. **First flash attempt aborted before touching the boards.** 22:09:50: `idf.py flash`
   re-ran cmake without the IDF toolchain on PATH and failed (`CMAKE_ASM_COMPILER not
   found`); no serial port was opened and no board was reset. The retry was launched from an
   activated v6.0.2 environment. The manifest records both commands.

## Reporting fields at seal (declared now, before results)
- `n_completed` unit = one BENCH58 transmission attempt by one board (expected 160 = 80 x 2);
  the actual count is taken from the split logs and recorded in the manifest notes.
- `n_excluded` = 0 by the exclusion rule above ("None"). The exclusion field is a **rule**,
  not a count: an analyzer must be able to re-derive the same set from the logs.

## Results (appended 2026-10-19, before sealing)

### Execution
- Flash: the driver `idf.py flash` path (460800 baud) failed three times mid-app-image
  (`A fatal error occurred: No more data to read from the serial port`, rc=2; logged in
  `flash_COM3.log`). Both boards were then flashed manually with esptool at 115200
  (`write-flash`, dio/40m, offsets 0x1000/0x8000/0x10000); esptool reported the image hash
  verified on both boards. The manual flash output was console-only (not captured to a file) --
  recorded as a provenance gap in the manifest.
- Capture: `ceepew_monitor.py --port COM3 --port COM4 --baud 115200 --duration 480` ->
  `capture_dual.log` (3,095,992 B); split -> `capture_DEVICE_A.log` (93 ring rows) /
  `capture_DEVICE_B.log` (97 ring rows).
- Health: no panic / Guru / abort / assert / watchdog reset in either split log (only the boot
  `rst:0x1 (POWERON_RESET)` line). One BLE warning:
  `W (11987) transport_ble: adv watchdog: advertising down for 2473 ms - forcing re-arm
  (phase=SIGN_PK_EXCHANGE state=connected)`. BLE lock-fix fingerprint
  `adv re-arm deferred to post-unlock`: 0 hits (the deferral path was not exercised).
- Campaign: `[BENCH58_TX] seq=0..79` then `[BENCH58_DONE] sent=80` on BOTH boards -> 160/160
  planned transmissions attempted (unit = one BENCH58 TX attempt by one board).

### Conversion + analysis (manual; driver pcap step skipped)
The driver expects `capture_COM3.log`/`capture_COM4.log` but the splitter writes
`capture_DEVICE_A/B.log`, so its pcap step silently skips these logs. Conversion/analysis were
run manually (command records appended to the manifest, marked reconstructed=1):
- `serial_to_pcap.py` -> A: 93 frames, B: 94 frames.
- `analyze_pcap.py` -> airtime cross-checks all PASS.
- `mode_report.py` -> `data/derived/E1_A_plain_20261019/DEVICE_{A,B}/mode_report.{json,txt}`.

### Pre-registered acceptance checks
1. At least one captured frame addressed to/from a DUT MAC -> **PASS**.
   A pcap: 93 frames = 91 from DUT B + 2 non-DUT rows; B pcap: 94 frames = 91 from DUT A +
   3 non-DUT rows. Data classes (mode_report histograms): 118 B and 122 B dominate
   (47+34 and 49+32), plus 45/52/55/56 B control and 117/121 B rendezvous-class frames.
2. At least one frame with a 0x41 run >= 4 at the ESP-NOW payload -> **FAIL**.
   Max 0x41 run = 1 byte in every captured frame, both devices. This check cannot pass on the
   current pipeline (see next section).
3. mode_report verdict recorded verbatim (both devices):
   `VERDICT: application plaintext is NOT VISIBLE on air in this mode.`
   (payload offset 28; pooled payload 7953 B (A) / 8083 B (B); entropy 6.7865 / 6.7927 bits/B;
   printable-ASCII share 37.26% / 37.15%; marker hits 0/93 and 0/94.)

Per the pre-registered rule, failure of check (2) is reported as an **apparatus/design failure
of the acceptance test** -- NOT a negative result about the Mode-A claim.

### Why check (2) cannot pass (source + on-air evidence, not assumption)
- `main/session_send.c`: Step 1 runs `compress_huffman_compress()` UNCONDITIONALLY on the whole
  message (line ~456) before fragmentation; per fragment the pipeline is encrypt ->
  `stage_tx_frame_fec` (Hamming(15,11) + trailer) -> `stage_tx_esl_wrap`. The payload that
  reaches the radio is Huffman-coded and Hamming-coded; literal application bytes can never
  appear on air in this build.
- FEC parity positions are permuted per session from the session key, so even the un-encrypted
  Mode-A payload is not trivially decodable without session material.
- On-air layering (tshark -V): [24 B 802.11 action header][7f 18 fe 34][11 B ESP-NOW
  encapsulation][ARQ 1 B][CEE-PEW payload]. Positive control: rendezvous/control frames DO carry
  the cleartext ESL header (`45 53 ...`) on air at the payload boundary; Mode-A data frames have
  no ESL magic at that boundary (payload begins 0x90, the fragment/FEC stream) -> the
  ESL/encryption bypass is effective, matching `CONFIG_CEEPEW_CAPTURE_MODE_PLAIN=1`.
- Consequence: the pool report "payload offset 28" is the vendor-content start, not the CEE-PEW
  payload start, and the literal-marker method cannot discriminate encrypted from unencrypted
  payloads whenever compression/FEC are enabled.

### Disposition
- Classification: **apparatus failure (acceptance-test design); run otherwise clean** (160/160 TX,
  no crashes, DUT frames present in both captures).
- Established: DUT-frame capture works at scale; cleartext ESL headers are visible on air in
  control frames; Mode-A data frames are not ESL-wrapped; no literal application bytes appear on
  air (by construction of compression+FEC).
- NOT established: whether the Mode-A payload is recoverable/readable by a passive observer. That
  needs a pre-declared FEC/compression-aware acceptance test (or a bench variant with those stages
  disabled) before any rerun.
- Author flag (no paper edits made): `README.md` "Mode A: 100% payload visible" remains
  unsupported -- all three Mode-A artifacts to date show no literal cleartext (20261017: 1 frame,
  0 markers; 20261018: 0 DUT frames; 20261019: 91 peer-DUT frames per capture, 0 literal markers,
  payload coded by design).

### Provenance gaps / observations (not exclusions)
- `flash_COM4.log` (listed in expected_raw_files) was never captured; `flash_COM3.log` holds the
  three failed 460800 attempts, not the successful manual flash.
- 7 rows of `capture_DEVICE_B.log` failed an independent strict length re-check; the pcap
  converter retained 94 frames and mode_report analysed 94 (A: 93/93).
- 2 (A) + 3 (B) captured rows are not DUT traffic (locally-administered source MACs `fd:3f:...`);
  excluded from analysis by the DUT-MAC rule (rule-based, re-derivable).
- The 118 B vs 122 B class split was not resolved to specific campaign variants in this pass; all
  frames were kept in the census.

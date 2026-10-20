# E2: Cleartext Injection Test — Run Plan

## Objective
Verify that CEE-PEW rejects **unencrypted ESP-NOW frames** at the ESL stage when the receiver expects CEE-PEW framing.

## Threat Model
An attacker (or compromised peer) sends:
1. **Plain ESP-NOW payload** — random bytes, no CEE-PEW framing
2. **Correct header + wrong tag** — valid ESL magic/CRC but invalid Ascon tag
3. **Correct header/CRC + random body** — passes early checks, fails at box/ascon

## Test Matrix

| Case | Sender Build | Receiver Build | Expected Result |
|------|--------------|----------------|-----------------|
| E2(a) plain payload | `build_cap_plain` | `build_cap_ceepew` | ESL stage reject (`err=ESL_ERR_*`) |
| E2(b) valid hdr, bad tag | `build_cap_ceepew` (forge-armed) | `build_cap_ceepew` | Ascon tag reject (`err=5`) |
| E2(c) valid hdr+CRC, random body | custom injection | `build_cap_ceepew` | Box/Ascon reject |

**Note:** Case E2(b) is **already exercised** by the BENCH58 forge path (every 10th frame corrupts a box byte). Today's earlier run showed 10/10 forged frames rejected.

## Hardware Setup
- **Node A (sender)**: COM3 (DEVKIT v1, GPIO14 on the shared E5 sync line)
- **Node B (receiver)**: COM5 (NodeMCU, GPIO14 on the same shared E5 sync line)
- Distance: 20 cm, fixed orientation
- Channel: 1 (hopping suppressed in capture builds)

## Procedure

### E2(a): Plain → CEE-PEW
1. Flash `build_cap_plain` to COM3
2. Flash `build_cap_ceepew` to COM5
3. Run dual-port monitor at 921600 baud for 300 s
4. Parse logs for:
   - COM3: `[BENCH58_TX]` lines (80 frames expected)
   - COM5: `[SECURE_CHAT_RX] Discard: ESL transport header/MAC/replay check failed`
5. **Pass criterion**: 0 frames reach "Message received" on COM5

### E2(b): Forged frames (BENCH58 built-in)
1. Flash `build_cap_ceepew` to **both** boards
2. Run dual-port monitor for 300 s (BENCH58 auto-runs)
3. Parse logs for:
   - COM3: `[BENCH58_TX] seq=X forged` (8 frames expected: seq 9,19,29,...,79)
   - COM5: `[SECURE_CHAT_RX] Discard: Ascon-128 AEAD auth tag verification FAILED (err=5)`
4. **Pass criterion**: forged_count_TX == ascon_reject_RX, and 0 forged frames reach app layer

### E2(c): Window poisoning (requires injection firmware)
**BLOCKED** — needs `CONFIG_CEEPEW_BENCH_INJECT` firmware to send arbitrary seq numbers.
- Planned: send frame with seq = s_max + 1000
- Expected: replay-window reject at commit stage

## Logging
- Combined dual-port log: `data/raw/E2_*/capture_dual.log`
- Split per-port: `capture_COM3.log`, `capture_COM5.log`
- Manifest: `data/raw/E2_*/manifest.json` with exclusion rules written **before** run

## Exclusion Rules (written pre-run)
1. Hardware-ACK subtype 0x1d frames are excluded from length/entropy analysis (counted separately)
2. Truncated captures (<28 B) are flagged as `non_full_size` but NOT excluded from rejection counts
3. No frame excluded for being a timing outlier
4. Capture-integrity gap = TX count (BENCH58_TX) minus captured frames, reported with cause

## Evidence Classes
| Claim | Evidence Class | Source |
|-------|----------------|--------|
| "100% forged frames rejected" | M-HW | Serial log + SHA-256 manifest |
| "0 forged frames reached app layer" | M-HW | "Message received" count vs forge count |
| "ESL stage rejects plain frames" | M-HW | ESL discard log line |

## Dependencies
- [ ] build_cap_ceepew completes (blocked on DRAM fix)
- [ ] msgstore fix merged (task_ui.c include fix applied)
- [ ] E1 capture runs complete first (plain/CCMP/CEE-PEW baselines)

## Next Steps After E2
1. E3: Replay test (needs injection firmware or saved-payload replay path)
2. E4(a): Forged-frame per-stage timing (BENCH58 already provides this)
3. E5: Sync-pulse latency (needs GPIO toggle firmware + wire connected)
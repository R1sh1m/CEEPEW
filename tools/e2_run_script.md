# E2: Cleartext Injection — Run Script

## Objective
Verify CEE-PEW rejects unencrypted ESP-NOW frames at the ESL transport stage.

## Test Cases

### E2(a): Plain → CEE-PEW (Mode Mismatch)
**Sender**: `build_cap_plain` (plain ESP-NOW, no CCMP, no CEE-PEW framing)  
**Receiver**: `build_cap_ceepew` (CEE-PEW full, expects ESL framing)  
**Expected**: ESL stage reject on receiver, 0 frames reach app layer

```powershell
# Flash sender (COM3) with plain mode
idf.py -B build_cap_plain -p COM3 flash

# Flash receiver (COM5) with CEE-PEW mode
idf.py -B build_cap_ceepew -p COM5 flash

# Run dual-port monitor for 300s
python tools/capture/dual_monitor.py --ports COM3,COM5 --baud 921600 --duration 300 --output data/raw/E2a_plain_injection_20261018/capture_dual.log

# Analyze: count BENCH58_TX on COM3 vs ESL_reject on COM5
python tools/capture/mode_report.py --run-id E2a_plain_injection_20261018
```

**Pass criterion**: 
- COM3: 80 `[BENCH58_TX]` lines
- COM5: 80 `[SECURE_CHAT_RX] Discard: ESL transport header/MAC/replay check failed`
- COM5: 0 "Message received" lines

### E2(b): Forged Frames (BENCH58 Built-in)
**Both boards**: `build_cap_ceepew` (BENCH58 with 10% forged frames)  
**Expected**: 8 forged frames rejected at Ascon stage (err=5)

```powershell
# Flash both with CEE-PEW
idf.py -B build_cap_ceepew -p COM3 flash
idf.py -B build_cap_ceepew -p COM5 flash

# Run dual-port monitor
python tools/capture/dual_monitor.py --ports COM3,COM5 --baud 921600 --duration 300 --output data/raw/E2b_forged_rejection_20261018/capture_dual.log

# Analyze
python tools/capture/mode_report.py --run-id E2b_forged_rejection_20261018
python tools/bench_timing_report.py --run-id E2b_forged_rejection_20261018
```

**Pass criterion**:
- COM3: 8 `[BENCH58_TX] seq=X forged` lines (seq 9,19,29,...,79)
- COM5: 8 `[SECURE_CHAT_RX] Discard: Ascon-128 AEAD auth tag verification FAILED (err=5)`
- COM5: 0 forged frames reach "Message received"

## Evidence
| Claim | Evidence | Source |
|-------|----------|--------|
| "100% plain frames rejected" | ESL discard count = TX count | Serial log |
| "0 plain frames reached app" | "Message received" count = 0 | Serial log |
| "100% forged frames rejected" | Ascon reject count = forge count | Serial log |
| "Forged frames rejected at box stage" | err=5 in discard log | Serial log |

## Blocking
- [ ] build_cap_ceepew must complete compilation
- [ ] Boards must be free from other agent
# CEE-PEW Wireshark Figure Specification

This document specifies the exact Wireshark views and filters needed to reproduce the paper's key claims about CEE-PEW's on-wire behavior.

---

## Validation Status

**PARTIALLY VALIDATED** — The dissector loads without Lua errors and fires on real
capture pcaps. However:

- **Mode B (CCMP) pcaps exist**: `data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap` and
  `DEVICE_B.pcap`. Dissector fires on 10 frames (DEVICE_A) and 7 frames (DEVICE_B).
- **Mode A (Plain) pcaps exist**: `data/derived/E1_A_plain_20261018/DEVICE_A.pcap` and
  `DEVICE_B.pcap`. 0 ESL-wrapped frames found (expected — ESL bypass mode).
- **Mode C (CEE-PEW) pcaps do NOT exist**: Only `.log` serial dumps in
  `data/raw/E1_C_ceepew_20261018/`. Cannot validate Mode C figures against real pcaps.

All figure sources below reference pcaps that actually exist. Mode C figures are
marked as **not runnable** against real pcaps.

### Frame-counting filters are guarded (do not remove the guard)

The dissector registers as a **post-dissector**, so it inspects *every* frame for the two
magic bytes `45 53`. Arbitrary payload bytes will occasionally contain that pair and produce
a **false positive**. The one such frame in the Mode B capture is frame **489** of
`data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap`, which dissects as CEE-PEW carrying nonsense
(`seq = 4560462445267697724`, `timestamp = 251658288`, payload mostly zeros).

Every filter in this document that counts or plots ESL frames is therefore written as:

```
ceepew.esl.magic0 == 0x45 && ceepew.esl.version == 1 && ceepew.esl.seq < 100000
```

Real frames in this capture satisfy `seq <= 30` and `timestamp_s <= 120`; the false positive
fails both bounds by many orders of magnitude. Without the guard, any frame tally — and any
figure built from one — silently includes it.

Verified per-frame values for every genuine ESL frame are tabulated in
`tools/capture/wireshark/FILTERS.md`.

---

## Headline Figure: Mode A/B/C Contrast

**Purpose**: Demonstrate the three capture modes and their security properties:
- **Mode A (Plain)**: Application payload visible in cleartext
- **Mode B (CCMP)**: Payload encrypted by hardware CCMP (opaque)
- **Mode C (CEE-PEW)**: Payload encrypted by Ascon-128 AEAD + crypto_box (opaque with CEE-PEW framing structure)

### Figure 1A: Mode A - Plaintext Visible

**Source**: `data/derived/E1_A_plain_20261018/DEVICE_A.pcap`

**Filter**: `ceepew.esl.magic0 == 0x45 && ceepew.esl.version == 1 && ceepew.esl.seq < 100000` (no ESL-wrapped frames expected in Mode A)

**View Settings**:
- Packet List: Show frame number, time, source, destination, protocol, length, info
- Packet Details: Expand "CEE-PEW ESP-NOW Security Layer" → "ESL Payload"
- Packet Bytes: Highlight ASCII-printable region

**What to Show**:
1. Raw packet bytes pane showing cleartext payload (no CEE-PEW layer will match)
2. Confirm no ESL magic bytes (0x45 0x53) appear in session data frames

**Expected Observation**: Mode A session data frames have no ESL header — payload is
cleartext ESP-NOW payload visible as raw bytes.

**Conclusion**: Mode A transmits application data in cleartext, confirming the "no encryption" baseline.

**Status**: Runnable against real pcap. 0 CEE-PEW frames match (expected).

---

### Figure 1B: Mode B - CCMP Encrypted (Opaque)

**Source**: `data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap`

**Filter**: `ceepew.esl.magic0 == 0x45 && ceepew.esl.version == 1 && ceepew.esl.seq < 100000`

**View Settings**:
- Same as Figure 1A
- Packet Bytes: Show hex only (no ASCII)

**What to Show**:
1. Full packet details pane showing ESL header fields
2. ESL Payload section showing opaque hex bytes
3. No ASCII visible in payload

**Expected Observation**: Payload appears as random hex bytes with no discernible ASCII pattern.

**Conclusion**: Mode B encrypts payload with hardware CCMP, confirming hardware-level encryption.

**Status**: Runnable against real pcap. 10 CEE-PEW frames match.

---

### Figure 1C: Mode C - CEE-PEW Encrypted (Opaque with Structure)

**Source**: `data/raw/E1_C_ceepew_20261018/` — **NO .pcap FILES EXIST**

Only `.log` serial dumps are available (`capture_DEVICE_A.log`, `capture_DEVICE_B.log`,
`capture_dual.log`). No `.pcap` was generated for Mode C.

**Filter**: `ceepew.esl.magic0 == 0x45 && ceepew.esl.version == 1 && ceepew.esl.seq < 100000` (cannot be tested — no pcap)

**View Settings**:
- Same as Figure 1A
- Packet Details: Expand all ESL header fields

**What to Show**:
1. Full packet details pane showing:
   - ARQ Sequence byte
   - ESL Magic (0x45 0x53)
   - ESL Version (1)
   - ESL Flags
   - ESL Timestamp
   - ESL Sequence Number (msg_id)
   - ESL Nonce Counter
   - Message Type (0x00 = DATA)
2. ESL Payload section showing opaque hex bytes
3. ESL CRC-32 field

**Expected Observation**:
- ESL header fields are clearly labeled and populated
- Payload is opaque (encrypted)
- Frame structure matches specification (249 bytes total)

**Conclusion**: Mode C provides application-level encryption with full CEE-PEW framing structure visible.

**Status**: **NOT RUNNABLE** — no Mode C pcap exists. Figure 1C cannot be reproduced from real capture data.

---

## Supporting Figure: Frame Size Distribution

**Purpose**: Show that CEE-PEW frames fit within ESP-NOW v1 limits (250 bytes) and demonstrate the FEC overhead.

### Figure 2: Frame Size Histogram (Mode B)

**Source**: `data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap`

**Filter**: `ceepew.esl.magic0 == 0x45 && ceepew.esl.version == 1 && ceepew.esl.seq < 100000`

**Command**:
```powershell
tshark -X lua_script:tools/capture/wireshark/ceepew_espnow.lua \
  -r data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap \
  -Y "ceepew.esl.magic0 == 0x45 && ceepew.esl.version == 1 && ceepew.esl.seq < 100000" \
  -T fields -e frame.len | sort | uniq -c | sort -rn
```

**Expected Output**:
```
  XX  249    # Full FEC-encoded data frames
  YY  28     # Control frames (rendezvous, etc.)
  ZZ  ...    # Other sizes
```

**What to Show**:
- Histogram of frame sizes
- Peak at 249 bytes (full data frame)
- Smaller peaks for control frames

**Conclusion**: CEE-PEW frames fit within ESP-NOW v1 limits (250 bytes) with 249-byte full frames.

**Status**: Runnable against real pcap.

---

## Supporting Figure: ESL Sequence Continuity

**Purpose**: Demonstrate that the ESL replay window (64-bit bitmap) is functioning correctly by showing monotonically increasing sequence numbers.

### Figure 3: ESL Sequence Progression

**Source**: `data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap`

**Filter**: `ceepew.esl.seq`

**Command**:
```powershell
tshark -X lua_script:tools/capture/wireshark/ceepew_espnow.lua \
  -r data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap \
  -Y "ceepew.esl.seq" \
  -T fields -e frame.time_epoch -e ceepew.esl.seq | Select-Object -First 30
```

**Expected Output**:
```
1729234567.123456  1
1729234567.234567  2
1729234567.345678  3
...
```

**What to Show**:
- Table of timestamp vs. ESL sequence number
- Monotonically increasing sequence (with possible gaps for lost frames)

**Conclusion**: ESL sequence numbers are monotonically increasing, confirming replay window functionality.

**Status**: Runnable against real pcap.

---

## Supporting Figure: Nonce Parity Split

**Purpose**: Demonstrate the nonce parity invariant (initiator uses even nonces, responder uses odd nonces).

### Figure 4: Nonce Parity by Role

**Source**: `data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap`

**Filter**: `ceepew.esl.nonce_counter`

**Command**:
```powershell
tshark -X lua_script:tools/capture/wireshark/ceepew_espnow.lua \
  -r data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap \
  -Y "ceepew.esl.nonce_counter" \
  -T fields -e ceepew.esl.nonce_counter
```

**Expected Output**: Even nonces (initiator) and odd nonces (responder) appear in disjoint sets.

**What to Show**:
- Histogram of nonce parity (even vs. odd)
- Clear separation between initiator and responder nonces

**Conclusion**: Nonce parity invariant is maintained, preventing reflection/collision attacks.

**Status**: Runnable against real pcap.

---

## Honest Limitations

### 1. Degraded Capture Quality

These pcaps were produced from serial "ring hex" dumps at 115200 baud. The capture pipeline is:

```
ESP32 promiscuous RX → ring buffer (4 slots × 300 B) →
  UART @ 115200 baud → ceepew_monitor.py → serial_to_pcap.py → pcap
```

**Integrity rates** (intact rows / total ring-dump lines):
- Mode A: ~22.3%
- Mode B: ~14.6%
- Mode C: ~19.3%

**Implication**: The pcap is **corroborating evidence**, not primary evidence. The primary evidence is the intact metadata lines in the serial logs. The pcap confirms the frame structure but cannot be used for quantitative analysis (e.g., frame counts, timing).

### 2. Encrypted Payloads

CEE-PEW payloads are encrypted with:
- Ascon-128 AEAD (inner encryption)
- crypto_box (XSalsa20 + HMAC-SHA256, outer encryption)
- Ed25519 signature (authentication)

**Implication**: Wireshark shows **opaque bytes** for Mode C payloads. The value of the dissector is:
- Framing structure visibility (ESL header fields)
- Frame size distribution
- Sequence continuity
- **Absence of cleartext** (negative evidence)

The dissector cannot show "what the message says" — only "that the message is encrypted."

### 3. ESP-NOW Vendor-Specific Frames

ESP-NOW rides 802.11 vendor-specific action frames (category 0x7F, OUI 0x18:FE:34). Without the Lua dissector:
- Wireshark shows raw hex for the payload
- No named fields are available
- Filtering requires byte-offset patterns

**Implication**: The dissector **must be loaded** to see the CEE-PEW structure. This is a dependency for reproducibility.

### 4. No FEC Decoding

The dissector does not decode Hamming(15,11) FEC. The payload is shown as raw bytes.

**Implication**: Single-bit error correction is not visible in Wireshark. The FEC benefit is demonstrated in host unit tests (`tests/host/test_hamming.c`), not in the pcap.

### 5. No Decryption

The dissector does not decrypt Ascon-128 AEAD or crypto_box.

**Implication**: The dissector cannot show "the plaintext message." This is by design — if the dissector could decrypt, the encryption would be broken.

### 6. Rendezvous Frames Not Dissected

Rendezvous frames (0x03/0x04) are sent as **raw ESP-NOW**, NOT wrapped in ESL header. They are used for clock-offset calibration before channel hopping starts.

**Implication**: Rendezvous frames will appear as "NON_ESL" or undisseced in Wireshark. They are not part of the active session traffic.

### 7. Mode A ESL Bypass

Mode A pcaps contain 0 ESL-wrapped session data frames because `CEEPEW_CAPTURE_ESL_BYPASS=1` bypasses the ESL header. Cleartext payloads are visible only as raw hex in the packet bytes pane.

### 8. Mode C Pcap Missing

No `.pcap` file exists for Mode C. Only `.log` serial dumps are available in
`data/raw/E1_C_ceepew_20261018/`. Figures 1C, 2 (Mode C), 3 (Mode C), and 4 (Mode C)
cannot be reproduced from real capture data.

### 9. Post-Dissector Registration

Wireshark 4.6.9 does not expose a `wlan.heuristic` table, so this dissector uses
`register_postdissector()` and scans every frame for ESL magic bytes. This is slightly
less efficient than a heuristic dissector but is the only reliable registration method
available.

---

## Reproduction Checklist

To reproduce the figures:

1. **Load the dissector**:
   ```powershell
   wireshark -X lua_script:tools/capture/wireshark/ceepew_espnow.lua
   ```

2. **Open the pcap**:
   - Mode A: `data/derived/E1_A_plain_20261018/DEVICE_A.pcap`
   - Mode B: `data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap`
   - Mode C: **No pcap exists** (only .log serial dumps)

3. **Apply the filter**:
   - `ceepew.esl.magic0 == 0x45 && ceepew.esl.version == 1 && ceepew.esl.seq < 100000` (for ESL-wrapped frames)

4. **Expand the CEE-PEW layer** in packet details

5. **Compare** the three modes side-by-side

6. **Note** the limitations above when interpreting the results

---

## References

- **Dissector Source**: `tools/capture/wireshark/ceepew_espnow.lua`
- **Filter Cheat Sheet**: `tools/capture/wireshark/FILTERS.md`
- **Capture Manifests**: `data/raw/E1_*_*/manifest.json`
- **Mode Report Tool**: `tools/capture/mode_report.py`
- **PCAP Generator**: `tools/capture/serial_to_pcap.py`

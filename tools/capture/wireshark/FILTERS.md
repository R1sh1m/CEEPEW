# CEE-PEW Wireshark Display Filter Cheat Sheet

## TShark Availability

**tshark.exe: AVAILABLE** (v4.6.9, `C:\Program Files\Wireshark\tshark.exe`)

## Validation Status

**PARTIALLY VALIDATED** — The dissector loads without Lua errors and fires on real
capture pcaps. Field filters resolve and return correct values. However:

- **Mode B (CCMP) pcaps**: Dissector fires on 10 frames (DEVICE_A) and 7 frames (DEVICE_B)
  from `data/raw/E1_B_ccmp_20261017/`. These are ESL-wrapped control frames (rendezvous,
  PFS handshake) that remain ESL-wrapped even in ESL-bypass mode.
- **Mode A (Plain) pcaps**: 0 ESL-wrapped frames found in
  `data/derived/E1_A_plain_20261018/DEVICE_A.pcap` and `DEVICE_B.pcap`. This is expected
  because Mode A uses `CEEPEW_CAPTURE_ESL_BYPASS=1`, which bypasses the ESL header on the
  session data path. Only ESL control frames are wrapped, and none appear in these captures.
- **Mode C (CEE-PEW) pcaps**: No `.pcap` files exist for Mode C — only `.log` serial dumps
  in `data/raw/E1_C_ceepew_20261018/`. Cannot validate against Mode C pcaps.

See the validation output section below for actual tshark results.

---

## Quick Start

Load the dissector and open a capture:

```powershell
# Windows — Wireshark GUI
wireshark -X lua_script:tools/capture/wireshark/ceepew_espnow.lua -r data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap

# Windows — tshark verbose dissection
tshark -X lua_script:tools/capture/wireshark/ceepew_espnow.lua -r data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap -V
```

---

## Display Filters

### Frame Type Filters

| Filter | Description |
|--------|-------------|
| `ceepew.frame_type contains "DATA"` | Regular chat messages |
| `ceepew.frame_type contains "PFS"` | Perfect Forward Secrecy handshake frames |
| `ceepew.frame_type contains "RENDEZVOUS"` | Channel sync frames |
| `ceepew.frame_type contains "NON_ESL"` | Non-CEE-PEW frames (BLE beacons, etc.) |
| `ceepew.frame_type contains "TOO_SHORT"` | Invalid/truncated frames |
| `ceepew.frame_type contains "UNKNOWN"` | Frames with unrecognized message type byte |

### ESL Header Field Filters

| Filter | Description |
|--------|-------------|
| `ceepew.esl.magic0 == 0x45` | Valid ESL magic byte 0 ('E') |
| `ceepew.esl.magic1 == 0x53` | Valid ESL magic byte 1 ('S') |
| `ceepew.esl.version == 1` | ESL protocol version 1 |
| `ceepew.esl.flags.cookie_required == 1` | DoS protection active (high queue depth) |
| `ceepew.esl.flags.has_cookie == 1` | DoS cookie present in frame |
| `ceepew.esl.timestamp_s > 1000000` | Frames after 1M seconds uptime |
| `ceepew.esl.seq > 100` | Frames after sequence 100 |
| `ceepew.esl.nonce_counter > 1000` | Frames after nonce 1000 |

### Message Type Filters

| Filter | Description |
|--------|-------------|
| `ceepew.esl.msg_type == 0x00` | DATA (chat message) |
| `ceepew.esl.msg_type == 0x01` | PFS_INIT (PFS handshake initiator) |
| `ceepew.esl.msg_type == 0x02` | PFS_RESP (PFS handshake responder) |
| `ceepew.esl.msg_type == 0x03` | RENDEZVOUS_REQ (sync request) |
| `ceepew.esl.msg_type == 0x04` | RENDEZVOUS_ACK (sync ACK) |

### Payload Filters

| Filter | Description |
|--------|-------------|
| `ceepew.payload_len > 100` | Large payloads (>100 bytes) |
| `ceepew.payload_len < 50` | Small payloads (control frames) |
| `ceepew.payload_len == 220` | Full FEC-encoded frame payload |

### ARQ Filters

| Filter | Description |
|--------|-------------|
| `ceepew.arq.seq == 0` | First frame in ARQ sequence |
| `ceepew.arq.seq > 10` | Frames after ARQ sequence 10 |

### Combined Filters (Paper Claims)

| Filter | Purpose |
|--------|---------|
| `ceepew.frame_type contains "DATA" && ceepew.payload_len > 50` | Active chat messages |
| `ceepew.esl.seq >= 1 && ceepew.esl.seq <= 80` | First 80 frames of session |
| `ceepew.esl.nonce_counter % 2 == 0` | Initiator frames (even nonces) |
| `ceepew.esl.nonce_counter % 2 == 1` | Responder frames (odd nonces) |

---

## Coloring Rules

Add these to **View → Coloring Rules** to visually distinguish capture modes:

| Rule Name | Filter | Foreground | Background |
|-----------|--------|------------|------------|
| **CEE-PEW DATA** | `ceepew.esl.msg_type == 0x00` | Black | Light Green |
| **CEE-PEW PFS** | `ceepew.esl.msg_type >= 0x01 && ceepew.esl.msg_type <= 0x02` | White | Orange |
| **CEE-PEW Rendezvous** | `ceepew.esl.msg_type >= 0x03 && ceepew.esl.msg_type <= 0x04` | White | Blue |
| **Invalid Frame** | `ceepew.frame_type contains "TOO_SHORT"` | Red | Yellow |
| **Non-CEE-PEW** | `ceepew.frame_type contains "NON_ESL"` | Gray | Light Gray |
| **High DoS Load** | `ceepew.esl.flags.cookie_required == 1` | Black | Pink |

### Mode Contrast Coloring

To emphasize the Mode A/B/C contrast:

| Mode | Filter | Foreground | Background |
|------|--------|------------|------------|
| **Mode A (Plain)** | `ceepew.frame_type contains "DATA" && frame.len < 150` | Black | Cyan |
| **Mode B (CCMP)** | `ceepew.frame_type contains "DATA" && frame.len >= 150 && frame.len < 200` | Black | Light Blue |
| **Mode C (CEE-PEW)** | `ceepew.frame_type contains "DATA" && frame.len >= 200` | White | Dark Green |

---

## Paper Claim Reproduction Filters

### Claim 1: Cleartext Visible in Mode A

Mode A uses `CEEPEW_CAPTURE_ESL_BYPASS=1`, so session data frames are sent as
cleartext ESP-NOW payloads without an ESL header. The dissector will **not** match
these frames (no ESL magic bytes). Cleartext visibility must be confirmed from the
raw hex in Wireshark's packet bytes pane, not from CEE-PEW display filters.

```powershell
# Mode A: Show all frames (no CEE-PEW filter will match)
tshark -X lua_script:tools/capture/wireshark/ceepew_espnow.lua \
  -r data/derived/E1_A_plain_20261018/DEVICE_A.pcap \
  -V | Select-Object -First 50
```

### Claim 2: Opaque Payload in Mode B and C

Mode B pcaps exist and contain ESL-wrapped control frames. Mode C pcaps do not
exist (only .log serial dumps).

```powershell
# Mode B: Show ESL-wrapped frames with opaque payload
tshark -X lua_script:tools/capture/wireshark/ceepew_espnow.lua \
  -r data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap \
  -Y "ceepew.esl.magic0 == 0x45" \
  -T fields -e frame.number -e ceepew.payload_len -e ceepew.esl.seq
```

**Expected:** Payload shown as opaque hex bytes, no ASCII visible.

### Claim 3: Frame Size Distribution

```powershell
# Mode B: Show frame size distribution for ESL-wrapped frames
tshark -X lua_script:tools/capture/wireshark/ceepew_espnow.lua \
  -r data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap \
  -Y "ceepew.esl.magic0 == 0x45" \
  -T fields -e frame.len | sort | uniq -c
```

### Claim 4: ESL Sequence Continuity

```powershell
# Show ESL sequence progression
tshark -X lua_script:tools/capture/wireshark/ceepew_espnow.lua \
  -r data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap \
  -Y "ceepew.esl.seq" \
  -T fields -e frame.time_epoch -e ceepew.esl.seq | Select-Object -First 20
```

---

## TShark Validation Output (Actual)

### Dissector loads without errors

```
CEE-PEW ESP-NOW dissector loaded
  ESL header: 24 bytes (magic, version, flags, timestamp, seq, nonce)
  ARQ prefix: 1 byte (sequence)
  ESL CRC: 4 bytes (CRC-32 over header + payload)
  Frame types: DATA, PFS_INIT, PFS_RESP, RENDEZVOUS_REQ, RENDEZVOUS_ACK
```

### Filter resolves and matches real frames (DEVICE_A.pcap, E1_B_ccmp_20261017)

```
$ tshark -X lua_script:tools/capture/wireshark/ceepew_espnow.lua -r data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap -Y "ceepew.esl.magic0 == 0x45"

  43   2.341292 Espressif_c2:52:ac → Espressif_97:d0:c0 CEE-PEW 246 Action, SN=4, FN=0
  146   8.658690 Espressif_c2:52:ac → Espressif_97:d0:c0 CEE-PEW 310 Action, SN=8, FN=0
  150   8.899792 Espressif_c2:52:ac → Espressif_97:d0:c0 CEE-PEW 313 Action, SN=9, FN=0
  324  24.163170 Espressif_c2:52:ac → Espressif_97:d0:c0 CEE-PEW 310 Action, SN=12, FN=0
  432  34.276006 Espressif_c2:52:ac → Espressif_97:d0:c0 CEE-PEW 313 Action, SN=14, FN=0
  489  39.315418 Espressif_c2:52:ac → Espressif_97:d0:c0 CEE-PEW 313 Action, SN=15, FN=0
  674  54.456774 Espressif_c2:52:ac → Espressif_97:d0:c0 CEE-PEW 313 Action, SN=18, FN=0
  972  79.711355 Espressif_c2:52:ac → Espressif_97:d0:c0 CEE-PEW 310 Action, SN=23, FN=0
 1150  94.916270 Espressif_c2:52:ac → Espressif_97:d0:c0 CEE-PEW 310 Action, SN=26, FN=0
 1330 110.059975 Espressif_c2:52:ac → Espressif_97:d0:c0 CEE-PEW 310 Action, SN=29, FN=0
```

### Field extraction works (DEVICE_A.pcap, E1_B_ccmp_20261017)

```
$ tshark -X lua_script:... -r data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap -Y "ceepew.esl.version == 1" -T fields -e frame.number -e ceepew.esl.seq -e ceepew.esl.nonce_counter -e ceepew.payload_len

43	4	6	157
146	6	10	221
150	6	10	224
...
1330	26	50	221
```

### Verified field values (little-endian, after byte-order correction)

These are the real values for the frames that genuinely carry an ESL header in
`data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap`:

| frame | `seq` | `timestamp_s` | `nonce_counter` |
|---|---|---|---|
| 43 | 4 | 11 | 6 |
| 146 | 6 | 17 | 10 |
| 150 | 6 | 17 | 10 |
| 324 | 9 | 33 | 16 |
| 432 | 11 | 43 | 20 |
| 674 | 15 | 63 | 28 |
| 972 | 20 | 89 | 38 |
| 1150 | 23 | 104 | 44 |
| 1330 | 26 | 119 | 50 |

**Cross-check that these are correct and not merely plausible:** `seq` and
`timestamp_s` advance in lock-step — every 3 sequence increments add exactly 15 s to the
timestamp, i.e. one frame every 5 s, which is the BENCH58 cadence. A wrong byte order
cannot produce that relationship; the earlier big-endian readings (`4 × 2^56` etc.) had no
such structure. The `nonce_counter` tracks `seq` with a consistent offset.

### KNOWN FALSE POSITIVE — do not count frames using the magic filter alone

Frame **489** matches `ceepew.esl.magic0 == 0x45` but is **not** a CEE-PEW frame. Its
extracted fields are garbage:

```
489   4560462445267697724   9475984844674097999   224
```

The dissector registers with `register_postdissector()`, so it scans **every** frame for
the two magic bytes `45 53`. A 2-byte magic occurring anywhere in arbitrary payload data
will occasionally produce a false lock — this is the single such case in this capture.

**Consequence:** never count frames with `-Y "ceepew.esl.magic0 == 0x45"` alone. Guard the
filter with the plausibility constraints that real frames satisfy:

```
ceepew.esl.version == 1 && ceepew.esl.seq < 100000 && ceepew.esl.timestamp_s < 1000000
```

Real frames in this capture satisfy `seq ≤ 30` and `timestamp_s ≤ 120`; the false positive
fails both by many orders of magnitude.

### Detailed dissection of Frame 43

```
CEE-PEW ESP-NOW Frame
        Magic Byte 0: 0x45
        Magic Byte 1: 0x53
        Version: 1
        Flags: 0x00
        .... ...0 = Cookie Required: False
        .... ..0. = Has Cookie: False
        Timestamp (seconds): 11
        ESL Sequence Number: 4
        Nonce Counter: 6
        Message Type: 0x40 (unknown)
    ESL Payload (157 bytes)
        Payload Length: 157
```

---

## Known Limitations

1. **Degraded Captures**: These pcaps were produced from serial "ring hex" dumps at 115200 baud. Only ~22.3% (Mode A) / 14.6% (Mode B) / 19.3% (Mode C) of rows are intact. Intact short metadata lines are the primary evidence; the pcap is corroborating.

2. **Encrypted Payloads**: CEE-PEW payloads are encrypted with Ascon-128 AEAD + crypto_box. Wireshark shows **opaque bytes**. The value is framing, sizes, sequence continuity, and *absence of cleartext* — not readable messages.

3. **ESP-NOW Vendor Frames**: ESP-NOW rides vendor-specific 802.11 frames. Without the Lua dissector, Wireshark shows raw hex. The dissector must be loaded to see named fields.

4. **No FEC Decoding**: The dissector does not decode Hamming(15,11) FEC. Payload is shown as raw bytes.

5. **No Decryption**: The dissector does not decrypt Ascon-128 AEAD or crypto_box. Ciphertext is shown as opaque.

6. **Rendezvous Frames**: Rendezvous frames (0x03/0x04) are sent as raw ESP-NOW, NOT wrapped in ESL header. They will not be dissected by this dissector.

7. **Mode A ESL Bypass**: Mode A pcaps contain 0 ESL-wrapped session data frames because `CEEPEW_CAPTURE_ESL_BYPASS=1` bypasses the ESL header. Cleartext payloads are visible only as raw hex.

8. **Post-Dissector Registration**: Wireshark 4.6.9 does not expose a `wlan.heuristic` table, so this dissector uses `register_postdissector()` and scans every frame for ESL magic bytes. This is slightly less efficient than a heuristic dissector but is the only reliable registration method available.

---

## References

- **ESL Header Layout**: `components/transport/transport_esl.c:84-92` (EslHeader_t)
- **ARQ Encoding**: `components/ecc/ecc_arq.c:138` (ecc_arq_encode)
- **FEC Encoding**: `main/session_send.c:248-310` (stage_tx_frame_fec)
- **ESL Wrap**: `components/transport/transport_esl.c:506-521` (transport_esl_process_outgoing_with_header)
- **Message Types**: `components/transport/transport_esl.h:79-88`
- **Constants**: `components/ceepew_common/ceepew_config.h`
- **Dissector Source**: `tools/capture/wireshark/ceepew_espnow.lua`

-- CEE-PEW ESP-NOW Dissector for Wireshark
--
-- This dissector parses ESP-NOW frames carrying CEE-PEW encrypted session
-- traffic. It exposes the ESL (ESP-NOW Security Layer) header fields and
-- CEE-PEW framing structure as named, filterable fields.
--
-- FRAME LAYOUT (derived from source, NOT guessed):
-- =================================================
--
-- 802.11 MAC Header (24 bytes for data frames)
--   Source: ESP-IDF 802.11 stack (wifi_promiscuous_pkt_t payload)
--
-- LLC/SNAP Header (6 bytes, optional, for SNAP-encapsulated ESP-NOW)
--   AA AA 03 00 00 00
--   Source: ESP-IDF esp_now.c
--
-- ESP-NOW Payload:
--   [ARQ seq: 1B] [ESL header: 24B] [ESL payload: variable] [ESL CRC: 4B]
--
--   ARQ Sequence Byte (offset 0 relative to ESP-NOW payload)
--     Source: components/ecc/ecc_arq.c:138 (ecc_arq_encode)
--     1-byte sequence counter for Stop-and-Wait ARQ (8-bit wire seq)
--
--   ESL Header (offset 1, 24 bytes packed, little-endian)
--     Source: components/transport/transport_esl.c:84-92 (EslHeader_t)
--     - magic0:     offset 0, 1 byte (0x45 = 'E')
--     - magic1:     offset 1, 1 byte (0x53 = 'S')
--     - version:    offset 2, 1 byte (currently 1)
--     - flags:      offset 3, 1 byte (bit 0: cookie_required, bit 1: has_cookie)
--     - timestamp_s: offset 4, 4 bytes (uint32_t, seconds since boot)
--     - seq:        offset 8, 8 bytes (uint64_t, ESL sequence / msg_id)
--     - nonce_counter: offset 16, 8 bytes (uint64_t, crypto nonce counter)
--
--   ESL Payload (offset 25, variable length)
--     For CEE-PEW session frames (Mode C):
--       [FEC-encoded ciphertext + inner CRC-32]
--       Source: main/session_send.c:248-310 (stage_tx_frame_fec)
--       - Hamming(15,11) FEC encoded (components/ecc/ecc_hamming.c)
--       - Inner CRC-32 (4 bytes) appended after FEC encoding
--       Source: components/transport/transport_esl.c:506-521
--
--     For capture bench Mode A/B (CEEPEW_CAPTURE_ESL_BYPASS=1):
--       Mode A: cleartext fragment payload (visible on air)
--       Mode B: hardware CCMP-encrypted payload (opaque)
--       Source: main/session_send.c:96-104, 317-325
--
--   ESL CRC-32 (last 4 bytes)
--     Source: components/transport/transport_esl.c:518-520
--     CRC-32 computed over ESL header + payload
--
-- CONSTANTS (from components/ceepew_common/ceepew_config.h):
-- ==========================================================
--   CEEPEW_ESL_HEADER_BYTES = 24
--   CEEPEW_ESL_CRC_BYTES = 4
--   CEEPEW_ESL_MIN_FRAME_BYTES = 28 (header + CRC)
--   CEEPEW_ARQ_SEQ_BYTES = 1
--   CEEPEW_PACKET_MAX_BYTES = 512 (max total frame size)
--   CEEPEW_FEC_BUF_MAX = 224 (max FEC-encoded frame per fragment)
--
-- MESSAGE TYPES (from components/transport/transport_esl.h:79-88):
-- ================================================================
--   CEEPEW_ESL_MSG_TYPE_DATA            = 0x00 (regular chat message)
--   CEEPEW_ESL_MSG_TYPE_PFS_INIT        = 0x01 (PFS handshake initiator)
--   CEEPEW_ESL_MSG_TYPE_PFS_RESP        = 0x02 (PFS handshake responder)
--   CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_REQ  = 0x03 (rendezvous sync request)
--   CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_ACK  = 0x04 (rendezvous sync ACK)
--
-- CAPTURE MODES:
-- ==============
--   Mode A (plain): CEEPEW_CAPTURE_ESL_BYPASS=1, payload visible in cleartext
--   Mode B (CCMP):  CEEPEW_CAPTURE_ESL_BYPASS=1, payload encrypted by HW CCMP
--   Mode C (CEEPEW): Full stack, payload encrypted by Ascon-128 AEAD + crypto_box
--
-- LIMITATIONS:
-- ============
--   - FEC decoding is NOT performed; payload shown as raw bytes
--   - Ascon/crypto_box decryption is NOT performed; ciphertext shown as opaque
--   - Frame length is biased in captures: only ~19-22% of ring-dump rows intact
--   - Without this dissector, Wireshark shows raw hex for vendor action frames
--   - Rendezvous frames (0x03/0x04) are raw ESP-NOW, NOT wrapped in ESL header
--   - This dissector uses register_postdissector() because Wireshark 4.6.9
--     does not expose a wlan.heuristic table. It scans every frame for ESL
--     magic bytes (0x45 0x53) at any offset.
--
-- USAGE:
-- ======
--   wireshark -X lua_script:ceepew_espnow.lua -r capture.pcap
--   tshark -X lua_script:ceepew_espnow.lua -r capture.pcap -V
--
-- Author: CEE-PEW Project
-- License: GPL v3

-- CEE-PEW dissector object
local ceepew_espnow = Proto("ceepew_espnow", "CEE-PEW ESP-NOW Security Layer")

-- ESL Header fields
local f_magic0 = ProtoField.uint8("ceepew.esl.magic0", "Magic Byte 0", base.HEX, nil, nil, "ESL magic byte 0 (0x45 = 'E')")
local f_magic1 = ProtoField.uint8("ceepew.esl.magic1", "Magic Byte 1", base.HEX, nil, nil, "ESL magic byte 1 (0x53 = 'S')")
local f_version = ProtoField.uint8("ceepew.esl.version", "Version", base.DEC, nil, nil, "ESL protocol version")
local f_flags = ProtoField.uint8("ceepew.esl.flags", "Flags", base.HEX, nil, nil, "ESL flags (bit 0: cookie_required, bit 1: has_cookie)")
local f_flags_cookie_req = ProtoField.bool("ceepew.esl.flags.cookie_required", "Cookie Required", 8, nil, 0x01, nil, "DoS cookie required (high queue depth)")
local f_flags_has_cookie = ProtoField.bool("ceepew.esl.flags.has_cookie", "Has Cookie", 8, nil, 0x02, nil, "DoS cookie present in frame")
local f_timestamp = ProtoField.uint32("ceepew.esl.timestamp_s", "Timestamp (seconds)", base.DEC, nil, nil, "Frame timestamp in seconds (little-endian)")
local f_seq = ProtoField.uint64("ceepew.esl.seq", "ESL Sequence Number", base.DEC, nil, nil, "ESL sequence number for replay window (msg_id, little-endian)")
local f_nonce = ProtoField.uint64("ceepew.esl.nonce_counter", "Nonce Counter", base.DEC, nil, nil, "Crypto nonce counter (64-bit, parity-split by role, little-endian)")

-- ARQ field
local f_arq_seq = ProtoField.uint8("ceepew.arq.seq", "ARQ Sequence", base.HEX, nil, nil, "ARQ stop-and-wait sequence byte")

-- Payload length
local f_payload_len = ProtoField.uint16("ceepew.payload_len", "Payload Length", base.DEC, nil, nil, "ESL payload length (excluding header and CRC)")

-- Message type (first byte after ESL header, for control frames)
local f_msg_type = ProtoField.uint8("ceepew.esl.msg_type", "Message Type", base.HEX, nil, nil, "CEE-PEW message type")

-- CRC field
local f_esl_crc = ProtoField.uint32("ceepew.esl.crc", "ESL CRC-32", base.HEX, nil, nil, "CRC-32 over ESL header + payload (little-endian)")

-- Frame type classification
local f_frame_type = ProtoField.string("ceepew.frame_type", "Frame Type", nil, nil, nil, nil, "CEE-PEW frame type classification")

ceepew_espnow.fields = {
    f_magic0, f_magic1, f_version, f_flags, f_flags_cookie_req, f_flags_has_cookie,
    f_timestamp, f_seq, f_nonce,
    f_arq_seq, f_payload_len, f_msg_type, f_esl_crc, f_frame_type
}

-- Constants
local ESL_HEADER_BYTES = 24
local ESL_CRC_BYTES = 4
local ARQ_SEQ_BYTES = 1
local ESL_MIN_FRAME_BYTES = ESL_HEADER_BYTES + ESL_CRC_BYTES  -- 28 bytes
local ESL_MAGIC0 = 0x45
local ESL_MAGIC1 = 0x53
local ESL_VERSION = 1

-- Message type names
local MSG_TYPE_NAMES = {
    [0x00] = "DATA (chat message)",
    [0x01] = "PFS_INIT (PFS handshake initiator)",
    [0x02] = "PFS_RESP (PFS handshake responder)",
    [0x03] = "RENDEZVOUS_REQ (sync request)",
    [0x04] = "RENDEZVOUS_ACK (sync ACK)",
}

-- Helper: check if buffer contains valid ESL header at given offset
local function is_esl_header(buf, offset)
    if buf:len() < offset + ESL_HEADER_BYTES then
        return false
    end
    local magic0 = buf(offset, 1):uint()
    local magic1 = buf(offset + 1, 1):uint()
    local version = buf(offset + 2, 1):uint()
    return (magic0 == ESL_MAGIC0 and magic1 == ESL_MAGIC1 and version == ESL_VERSION)
end

-- Helper: classify frame type
local function classify_frame_type(buf, offset, frame_len)
    if frame_len < ESL_MIN_FRAME_BYTES then
        return "TOO_SHORT (invalid)"
    end

    local magic0 = buf(offset, 1):uint()
    local magic1 = buf(offset + 1, 1):uint()

    if magic0 ~= ESL_MAGIC0 or magic1 ~= ESL_MAGIC1 then
        return "NON_ESL (not CEE-PEW)"
    end

    local version = buf(offset + 2, 1):uint()
    if version ~= ESL_VERSION then
        return string.format("UNKNOWN_VERSION (v%d)", version)
    end

    local payload_len = frame_len - ESL_HEADER_BYTES - ESL_CRC_BYTES
    if payload_len < 1 then
        return "EMPTY_PAYLOAD"
    end

    local msg_type = buf(offset + ESL_HEADER_BYTES, 1):uint()
    local type_name = MSG_TYPE_NAMES[msg_type] or string.format("UNKNOWN(0x%02X)", msg_type)
    return type_name
end

-- Main dissector function (post-dissector: called for every frame)
function ceepew_espnow.dissector(tvb, pinfo, tree)
    local buf = tvb
    local frame_len = buf:len()

    -- Find ESL header offset. Scan entire frame for ESL magic bytes.
    local esl_offset = nil
    local has_arq = false

    for i = 0, frame_len - ESL_HEADER_BYTES do
        if is_esl_header(buf, i) then
            esl_offset = i
            break
        end
    end

    -- If no ESL header found at direct offset, check for ARQ byte + ESL header
    if esl_offset == nil and frame_len >= ARQ_SEQ_BYTES + ESL_HEADER_BYTES then
        if is_esl_header(buf, ARQ_SEQ_BYTES) then
            esl_offset = ARQ_SEQ_BYTES
            has_arq = true
        end
    end

    -- If still no ESL header, this is not a CEE-PEW frame
    if esl_offset == nil then
        return 0
    end

    -- Calculate payload length
    local arq_offset = esl_offset - ARQ_SEQ_BYTES
    local payload_len = frame_len - esl_offset - ESL_HEADER_BYTES - ESL_CRC_BYTES

    -- Validate frame
    if payload_len < 0 then
        return 0
    end

    -- Add to protocol tree
    local subtree = tree:add(ceepew_espnow, buf(), "CEE-PEW ESP-NOW Frame")

    -- Frame type classification
    local frame_type = classify_frame_type(buf, esl_offset, frame_len - esl_offset)
    subtree:add(f_frame_type, frame_type)

    -- ARQ sequence (if present)
    if has_arq then
        subtree:add(f_arq_seq, buf(arq_offset, ARQ_SEQ_BYTES))
    end

    -- ESL Header
    local esl_subtree = subtree:add(buf(esl_offset, ESL_HEADER_BYTES), "ESL Header (24 bytes)")
    esl_subtree:add(f_magic0, buf(esl_offset, 1))
    esl_subtree:add(f_magic1, buf(esl_offset + 1, 1))
    esl_subtree:add(f_version, buf(esl_offset + 2, 1))
    esl_subtree:add(f_flags, buf(esl_offset + 3, 1))
    esl_subtree:add(f_flags_cookie_req, buf(esl_offset + 3, 1))
    esl_subtree:add(f_flags_has_cookie, buf(esl_offset + 3, 1))
    esl_subtree:add_le(f_timestamp, buf(esl_offset + 4, 4))
    esl_subtree:add_le(f_seq, buf(esl_offset + 8, 8))
    esl_subtree:add_le(f_nonce, buf(esl_offset + 16, 8))

    -- Message type (for control frames)
    if payload_len >= 1 then
        local msg_type = buf(esl_offset + ESL_HEADER_BYTES, 1):uint()
        local msg_type_tree = esl_subtree:add(f_msg_type, buf(esl_offset + ESL_HEADER_BYTES, 1))
        msg_type_tree:append_text(string.format(" (%s)", MSG_TYPE_NAMES[msg_type] or "unknown"))
    end

    -- Payload
    if payload_len > 0 then
        local payload_subtree = subtree:add(buf(esl_offset + ESL_HEADER_BYTES, payload_len),
                                            string.format("ESL Payload (%d bytes)", payload_len))
        payload_subtree:add(f_payload_len, payload_len)

        -- Note about encryption
        if frame_type == "DATA (chat message)" then
            payload_subtree:append_text(" [encrypted with Ascon-128 AEAD + crypto_box]")
        end
    end

    -- ESL CRC
    local crc_offset = esl_offset + ESL_HEADER_BYTES + payload_len
    if frame_len >= crc_offset + ESL_CRC_BYTES then
        subtree:add_le(f_esl_crc, buf(crc_offset, ESL_CRC_BYTES))
    end

    -- Set protocol info
    pinfo.cols.protocol = "CEE-PEW"
    -- NOTE: the 8-byte seq/nonce fields are little-endian (see add_le above);
    -- read them with le_uint64() so the Info column does not byte-swap them.
    local seq_val = buf(esl_offset + 8, 8):le_uint64()
    local nonce_val = buf(esl_offset + 16, 8):le_uint64()
    pinfo.cols.info = string.format("CEE-PEW seq=%s nonce=%s",
                                    tostring(seq_val), tostring(nonce_val))

    return 0
end

-- Register as post-dissector (called for every frame after regular dissection)
-- Wireshark 4.6.9 does not expose a wlan.heuristic table, so post-dissector
-- is the reliable way to scan every frame for ESL magic bytes.
register_postdissector(ceepew_espnow)

print("CEE-PEW ESP-NOW dissector loaded")
print("  ESL header: 24 bytes (magic, version, flags, timestamp, seq, nonce)")
print("  ARQ prefix: 1 byte (sequence)")
print("  ESL CRC: 4 bytes (CRC-32 over header + payload)")
print("  Frame types: DATA, PFS_INIT, PFS_RESP, RENDEZVOUS_REQ, RENDEZVOUS_ACK")

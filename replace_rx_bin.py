with open('main/task_session.c', 'rb') as f:
    content = f.read()

# Find exact byte sequences
start = content.find(b'RX data pipeline: CRC')
end = content.find(b'  err = CEEPEW_OK;\r\n\r\nrx_cleanup:')
if start < 0 or end < 0:
    print("Could not find markers")
    exit(1)

old_section = content[start:end + len(b'  err = CEEPEW_OK;\r\n\r\nrx_cleanup:')]
print(f"Old section length: {len(old_section)} bytes")

# New section (UTF-8 encoded)
new_section = b"""RX decrypt pipeline: CRC \xe2\x86\x92 FEC decode \xe2\x86\x92 decrypt \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\r
   * Run the 2-stage decrypt pipeline on the ESL-stripped payload.\r
   * Produces fragment payload (with 2-byte header) in region-allocated buffer. */\r
  if (!s_rx_decrypt_pipeline_built) {\r
    err = rxd_build_pipeline();\r
    if (err != CEEPEW_OK) {\r
      ESP_LOGE(TAG, \"[SECURE_CHAT_RX] Failed to build RX decrypt pipeline: %d\", (int)err);\r
      goto rx_cleanup;\r
    }\r
  }\r
\r
  uint8_t *frag_payload = NULL;\r
  uint16_t frag_payload_len = 0U;\r
  err = pipeline_run(&s_rx_decrypt_pipeline, &g_region,\r
                      local_work_frame, work_len,\r
                      &frag_payload, &frag_payload_len);\r
  if (err != CEEPEW_OK) {\r
    ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] Discard: RX decrypt pipeline failed (err=%d len=%u)\",\r
             (int)err, (unsigned)work_len);\r
    goto rx_cleanup;\r
  }\r
\r
  /* Fragment reassembly -------------------------------------------------------\r
   * Every message now carries a 2-byte fragment header:\r
   *   byte 0: 0x80 | (total_frags - 1)  (START bit + total-1 in lower 6 bits)\r
   *   byte 1: fragment index (0-based)\r
   * Followed by compressed chunk data.\r
   * ARQ delivers frames in strict order, so we expect sequential idx.\r
   * A message is complete when idx == total-1. */\r
  if (frag_payload_len < CEEPEW_FRAG_HEADER_BYTES) {\r
    ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] Discard: fragment payload too small (%u < %u)\",\r
             (unsigned)frag_payload_len, CEEPEW_FRAG_HEADER_BYTES);\r
    goto rx_cleanup;\r
  }\r
\r
  uint8_t flags = frag_payload[0];\r
  uint8_t frag_idx = frag_payload[1];\r
  bool is_start = (flags & 0x80U) != 0U;\r
  uint8_t total_frags = (uint8_t)((flags & 0x3FU) + 1U);\r
  uint16_t chunk_len = (uint16_t)(frag_payload_len - CEEPEW_FRAG_HEADER_BYTES);\r
\r
  /* Timeout check for partial assembly */\r
  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);\r
  if (s_frag_active && (now_ms - s_frag_start_ms > CEEPEW_FRAG_TIMEOUT_MS)) {\r
    ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] Fragment reassembly timeout (%u ms) \xe2\x80\x94 discarding partial\",\r
             CEEPEW_FRAG_TIMEOUT_MS);\r
    s_frag_active = false;\r
    s_frag_accum_len = 0U;\r
  }\r
\r
  if (is_start) {\r
    /* New message start (either first fragment or single-fragment message) */\r
    if (frag_idx != 0U) {\r
      ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] Discard: START fragment with idx=%u != 0\", (unsigned)frag_idx);\r
      goto rx_cleanup;\r
    }\r
    if (total_frags == 0U || total_frags > CEEPEW_MAX_FRAGMENTS) {\r
      ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] Discard: invalid total_frags=%u\", (unsigned)total_frags);\r
      goto rx_cleanup;\r
    }\r
    if (s_frag_active) {\r
      ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] New START while assembly active \xe2\x80\x94 discarding previous partial\");\r
    }\r
    s_frag_total = total_frags;\r
    s_frag_expected_idx = 0U;\r
    s_frag_accum_len = 0U;\r
    s_frag_active = true;\r
    s_frag_start_ms = now_ms;\r
  } else {\r
    /* Continuation fragment */\r
    if (!s_frag_active) {\r
      ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] Discard: continuation fragment without active assembly\");\r
      goto rx_cleanup;\r
    }\r
    if (frag_idx != s_frag_expected_idx) {\r
      ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] Discard: fragment idx=%u != expected=%u\",\r
               (unsigned)frag_idx, (unsigned)s_frag_expected_idx);\r
      s_frag_active = false;\r
      s_frag_accum_len = 0U;\r
      goto rx_cleanup;\r
    }\r
    if (total_frags != s_frag_total) {\r
      ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] Discard: fragment total=%u != expected=%u\",\r
               (unsigned)total_frags, (unsigned)s_frag_total);\r
      s_frag_active = false;\r
      s_frag_accum_len = 0U;\r
      goto rx_cleanup;\r
    }\r
  }\r
\r
  /* Accumulate compressed chunk */\r
  if (s_frag_accum_len + chunk_len > CEEPEW_HUFF_BUF_MAX) {\r
    ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] Discard: reassembly buffer overflow (%u + %u > %u)\",\r
             (unsigned)s_frag_accum_len, (unsigned)chunk_len, CEEPEW_HUFF_BUF_MAX);\r
    s_frag_active = false;\r
    s_frag_accum_len = 0U;\r
    goto rx_cleanup;\r
  }\r
  if (chunk_len > 0U) {\r
    memcpy(s_frag_accum + s_frag_accum_len, frag_payload + CEEPEW_FRAG_HEADER_BYTES, chunk_len);\r
  }\r
  s_frag_accum_len += chunk_len;\r
  s_frag_expected_idx++;\r
\r
  /* Check if message is complete */\r
  if (frag_idx == (uint8_t)(total_frags - 1U)) {\r
    ESP_LOGI(\"SESSION\", \"[SECURE_CHAT_RX] Message complete: %u fragments, %u compressed bytes\",\r
             (unsigned)total_frags, (unsigned)s_frag_accum_len);\r
\r
    /* Run decompress pipeline on reassembled compressed data */\r
    uint8_t *decoded_out = NULL;\r
    uint16_t decoded_len = 0U;\r
\r
    if (!s_rx_decompress_pipeline_built) {\r
      err = rxd_build_pipeline();\r
      if (err != CEEPEW_OK) {\r
        ESP_LOGE(TAG, \"[SECURE_CHAT_RX] Failed to build RX decompress pipeline: %d\", (int)err);\r
        s_frag_active = false;\r
        s_frag_accum_len = 0U;\r
        goto rx_cleanup;\r
      }\r
    }\r
\r
    err = pipeline_run(&s_rx_decompress_pipeline, &g_region,\r
                        s_frag_accum, s_frag_accum_len,\r
                        &decoded_out, &decoded_len);\r
    if (err != CEEPEW_OK) {\r
      ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] Discard: Huffman decompress failed (err=%d len=%u)\",\r
               (int)err, (unsigned)s_frag_accum_len);\r
      s_frag_active = false;\r
      s_frag_accum_len = 0U;\r
      goto rx_cleanup;\r
    }\r
\r
    /* Clear fragment state */\r
    s_frag_active = false;\r
    s_frag_accum_len = 0U;\r
\r
    /* Store peer MAC for UI display */\r
    {\r
      UIState_t ui_state;\r
      session_ui_get_state_snapshot(&ui_state);\r
      session_ui_ctx_lock();\r
      memcpy(g_ui_ctx.peer_mac, frame->src_mac, CEEPEW_DEVICE_ID_BYTES);\r
      g_ui_ctx.rssi_dbm = frame->rssi;\r
      g_ui_ctx.current_channel = frame->channel;\r
      session_ui_ctx_unlock();\r
    }\r
\r
    /* DOUBLE-ENDED POST-DERIVE SYNC ROUTING ------------------------------------\r
     * A successful round-trip decryption of a 1-byte sync payload is the\r
     * proof that crypto_box works in both directions with the converged\r
     * keys. Before routing to the sync handler, verify that the frame's\r
     * source MAC matches the GATT-verified peer WiFi MAC \xe2\x80\x94 this closes\r
     * the window where an attacker could relay encrypted sync frames\r
     * from a spoofed MAC address. */\r
    if (decoded_len == 1U && (decoded_out[0] == CEEPEW_KEY_SYNC_HELLO_BYTE ||\r
                              decoded_out[0] == CEEPEW_KEY_SYNC_ACK_BYTE)) {\r
      /* HARDWARE-GATED IDENTITY CHECK: verify frame src_mac matches the\r
       * WiFi MAC that was authenticated over the secure GATT channel. */\r
      CeePewErr_t mac_check = session_verify_wifi_mac_matches_frame(frame->src_mac);\r
      if (mac_check != CEEPEW_OK) {\r
        ESP_LOGW(\"SESSION\", \"[POST_DERIVE_SYNC] Sync frame WiFi MAC mismatch \xe2\x80\x94 discarding (possible relay)\");\r
        goto rx_cleanup;\r
      }\r
\r
      ESP_LOGI(\"SESSION\", \"[POST_DERIVE_SYNC] Received sync byte 0x%02X (%s)\",\r
               decoded_out[0], decoded_out[0] == CEEPEW_KEY_SYNC_HELLO_BYTE ? \"HELLO\" : \"ACK\");\r
\r
      CeePewErr_t sync_err = session_handle_key_sync_byte(decoded_out[0]);\r
      if (sync_err == CEEPEW_ERR_NEED_TX) {\r
        uint8_t ack_plain[1] = { CEEPEW_KEY_SYNC_ACK_BYTE };\r
        uint8_t peer_mac[6] = {0U};\r
        uint8_t peer_pk[32] = {0U};\r
        if (session_get_peer_wifi_mac(peer_mac) == CEEPEW_OK &&\r
            session_get_peer_public_key(peer_pk) == CEEPEW_OK) {\r
          CeePewErr_t send_ack_err = session_send_message(ack_plain, 1U, peer_mac, peer_pk);\r
          if (send_ack_err == CEEPEW_OK) {\r
            (void)session_confirm_ack_sent();\r
            ESP_LOGI(\"SESSION\", \"[POST_DERIVE_SYNC] Sent ACK to initiator %02X:%02X:%02X:%02X:%02X:%02X\",\r
                     peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);\r
          } else {\r
            ESP_LOGW(\"SESSION\", \"[POST_DERIVE_SYNC] Failed to send ACK: %d\", (int)send_ack_err);\r
          }\r
        }\r
      }\r
      (void)session_update_last_message_time();\r
      goto rx_cleanup;\r
    }\r
\r
    if (decoded_len == 1U && (decoded_out[0] == CEEPEW_KEY_SYNC_PING_BYTE ||\r
                              decoded_out[0] == CEEPEW_KEY_SYNC_PONG_BYTE)) {\r
      CeePewErr_t mac_check = session_verify_wifi_mac_matches_frame(frame->src_mac);\r
      if (mac_check != CEEPEW_OK) {\r
        ESP_LOGW(\"SESSION\", \"[KEEPALIVE] Keepalive frame WiFi MAC mismatch \xe2\x80\x94 discarding\");\r
        goto rx_cleanup;\r
      }\r
\r
      if (decoded_out[0] == CEEPEW_KEY_SYNC_PING_BYTE) {\r
        uint8_t pong_plain[1] = { CEEPEW_KEY_SYNC_PONG_BYTE };\r
        uint8_t peer_mac[6] = {0U};\r
        uint8_t peer_pk[32] = {0U};\r
        if (session_get_peer_wifi_mac(peer_mac) == CEEPEW_OK &&\r
            session_get_peer_public_key(peer_pk) == CEEPEW_OK) {\r
          (void)session_send_message(pong_plain, 1U, peer_mac, peer_pk);\r
        }\r
      }\r
      (void)session_update_last_message_time();\r
      goto rx_cleanup;\r
    }\r
\r
    /* Regular chat message: validate length and store */\r
    if (decoded_len > CEEPEW_MAX_MSG_BYTES) {\r
      ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] Discard: decompressed len %u > max %u\",\r
               (unsigned)decoded_len, CEEPEW_MAX_MSG_BYTES);\r
      goto rx_cleanup;\r
    }\r
\r
    err = msg_store_add(decoded_out, decoded_len, 0U);\r
    if (err != CEEPEW_OK) {\r
      ESP_LOGW(\"SESSION\", \"RX discard: msg_store_add failed (err=%d decoded=%u)\",\r
               (int)err, (unsigned)decoded_len);\r
      goto rx_cleanup;\r
    }\r
    /* Trigger RX LED blink (blue blink for 250 ms) */\r
    task_session_trigger_rgb_blink(RGB_BLUE_BLINK, 250U);\r
\r
    {\r
      UIEvent_t ui_event;\r
      memset(&ui_event, 0U, sizeof(ui_event));\r
      ui_event.type = UI_EVENT_MESSAGE_RECEIVED;\r
      ui_event.param = (uint32_t)msg_store_count();\r
      memcpy(ui_event.payload.message_rx.device_id, frame->src_mac,\r
             CEEPEW_DEVICE_ID_BYTES);\r
      ui_event.payload.message_rx.msg_id = (uint16_t)(msg_store_count() - 1U);\r
\r
      /* Depth-8 queue: xQueueOverwrite asserts (queue.c:938) on non-1 queues;\r
       * use xQueueSend and drop the event if the UI is backlogged. */\r
      BaseType_t q_rc = xQueueSend(g_ui_event_queue, &ui_event, 0U);\r
      if (q_rc != pdPASS) {\r
        ESP_LOGW(\"SESSION\", \"[SECURE_CHAT_RX] UI event queue full \xe2\x80\x94 dropped received-message event\");\r
      }\r
    }\r
\r
    (void)session_update_last_message_time();\r
\r
    s_stats.rx_frames_processed++;\r
  }\r
\r
  err = CEEPEW_OK;\r
\r
rx_cleanup:"""

if old_section in content:
    new_content = content[:start] + new_section + content[end + len(b'  err = CEEPEW_OK;\r\n\r\nrx_cleanup:'):]
    with open('main/task_session.c', 'wb') as f:
        f.write(new_content)
    print("Replacement successful!")
    print(f"New content length: {len(new_content)}")
else:
    print("Old section NOT found in content!")
    print(f"Old section starts with: {old_section[:100]}")
    print(f"Content at start: {content[start:start+100]}")
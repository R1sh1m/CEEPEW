# -*- coding: utf-8 -*-
with open('main/task_session.c', 'r', encoding='utf-8') as f:
    content = f.read()

old = """RX data pipeline: CRC → FEC decode → decrypt → decompress ────────
   * Run the 3-stage data pipeline on the ESL-stripped payload.
   * The pipeline produces the decompressed plaintext in a region-allocated
   * buffer. Build the pipeline once on first use. */
  if (!s_rx_data_pipeline_built) {
    err = rxd_build_pipeline();
    if (err != CEEPEW_OK) {
      ESP_LOGE(TAG, "[SECURE_CHAT_RX] Failed to build RX data pipeline: %d", (int)err);
      goto rx_cleanup;
    }
  }

  uint8_t *decoded_out = NULL;
  uint16_t decoded_len = 0U;
  err = pipeline_run(&s_rx_data_pipeline, &g_region,
                      local_work_frame, work_len,
                      &decoded_out, &decoded_len);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "[SECURE_CHAT_RX] Discard: RX data pipeline execution failed (err=%d len=%u)",
             (int)err, (unsigned)work_len);
    goto rx_cleanup;
  }

  /* Store peer MAC for UI display */
  {
    UIState_t ui_state;
    session_ui_get_state_snapshot(&ui_state);
    session_ui_ctx_lock();
    memcpy(g_ui_ctx.peer_mac, frame->src_mac, CEEPEW_DEVICE_ID_BYTES);
    g_ui_ctx.rssi_dbm = frame->rssi;
    g_ui_ctx.current_channel = frame->channel;
    session_ui_ctx_unlock();
  }

  /* ── DOUBLE-ENDED POST-DERIVE SYNC ROUTING ──────────────────────────
   * A successful round-trip decryption of a 1-byte sync payload is the
   * proof that crypto_box works in both directions with the converged
   * keys. Before routing to the sync handler, verify that the frame's
   * source MAC matches the GATT-verified peer WiFi MAC — this closes
   * the window where an attacker could relay encrypted sync frames
   * from a spoofed MAC address. */
  if (decoded_len == 1U && (decoded_out[0] == CEEPEW_KEY_SYNC_HELLO_BYTE ||
                            decoded_out[0] == CEEPEW_KEY_SYNC_ACK_BYTE)) {
    /* HARDWARE-GATED IDENTITY CHECK: verify frame src_mac matches the
     * WiFi MAC that was authenticated over the secure GATT channel. */
    CeePewErr_t mac_check = session_verify_wifi_mac_matches_frame(frame->src_mac);
    if (mac_check != CEEPEW_OK) {
      ESP_LOGW("SESSION", "[POST_DERIVE_SYNC] Sync frame WiFi MAC mismatch — discarding (possible relay)");
      goto rx_cleanup;
    }

    ESP_LOGI("SESSION", "[POST_DERIVE_SYNC] Received sync byte 0x%02X (%s)",
             decoded_out[0], decoded_out[0] == CEEPEW_KEY_SYNC_HELLO_BYTE ? "HELLO" : "ACK");

    CeePewErr_t sync_err = session_handle_key_sync_byte(decoded_out[0]);
    if (sync_err == CEEPEW_ERR_NEED_TX) {
      uint8_t ack_plain[1] = { CEEPEW_KEY_SYNC_ACK_BYTE };
      uint8_t peer_mac[6] = {0U};
      uint8_t peer_pk[32] = {0U};
      if (session_get_peer_wifi_mac(peer_mac) == CEEPEW_OK &&
          session_get_peer_public_key(peer_pk) == CEEPEW_OK) {
        CeePewErr_t send_ack_err = session_send_message(ack_plain, 1U, peer_mac, peer_pk);
        if (send_ack_err == CEEPEW_OK) {
          (void)session_confirm_ack_sent();
          ESP_LOGI("SESSION", "[POST_DERIVE_SYNC] Sent ACK to initiator %02X:%02X:%02X:%02X:%02X:%02X",
                   peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
        } else {
          ESP_LOGW("SESSION", "[POST_DERIVE_SYNC] Failed to send ACK: %d", (int)send_ack_err);
        }
      }
    }
    (void)session_update_last_message_time();
    goto rx_cleanup;
  }

  if (decoded_len == 1U && (decoded_out[0] == CEEPEW_KEY_SYNC_PING_BYTE ||
                            decoded_out[0] == CEEPEW_KEY_SYNC_PONG_BYTE)) {
    CeePewErr_t mac_check = session_verify_wifi_mac_matches_frame(frame->src_mac);
    if (mac_check != CEEPEW_OK) {
      ESP_LOGW("SESSION", "[KEEPALIVE] Keepalive frame WiFi MAC mismatch — discarding");
      goto rx_cleanup;
    }

    if (decoded_out[0] == CEEPEW_KEY_SYNC_PING_BYTE) {
      uint8_t pong_plain[1] = { CEEPEW_KEY_SYNC_PONG_BYTE };
      uint8_t peer_mac[6] = {0U};
      uint8_t peer_pk[32] = {0U};
      if (session_get_peer_wifi_mac(peer_mac) == CEEPEW_OK &&
          session_get_peer_public_key(peer_pk) == CEEPEW_OK) {
        (void)session_send_message(pong_plain, 1U, peer_mac, peer_pk);
      }
    }
    (void)session_update_last_message_time();
    goto rx_cleanup;
  }

  err = msg_store_add(decoded_out, decoded_len, 0U);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: msg_store_add failed (err=%d decoded=%u)",
             (int)err, (unsigned)decoded_len);
    goto rx_cleanup;
  }
  /* Trigger RX LED blink (blue blink for 250 ms) */
  task_session_trigger_rgb_blink(RGB_BLUE_BLINK, 250U);

  {
    UIEvent_t ui_event;
    memset(&ui_event, 0U, sizeof(ui_event));
    ui_event.type = UI_EVENT_MESSAGE_RECEIVED;
    ui_event.param = (uint32_t)msg_store_count();
    memcpy(ui_event.payload.message_rx.device_id, frame->src_mac,
           CEEPEW_DEVICE_ID_BYTES);
    ui_event.payload.message_rx.msg_id = (uint16_t)(msg_store_count() - 1U);

    /* Depth-8 queue: xQueueOverwrite asserts (queue.c:938) on non-1 queues;
     * use xQueueSend and drop the event if the UI is backlogged. */
    BaseType_t q_rc = xQueueSend(g_ui_event_queue, &ui_event, 0U);
    if (q_rc != pdPASS) {
      ESP_LOGW("SESSION", "[SECURE_CHAT_RX] UI event queue full — dropped received-message event");
    }
  }

  (void)session_update_last_message_time();

  s_stats.rx_frames_processed++;

  err = CEEPEW_OK;

rx_cleanup"""

new = """RX decrypt pipeline: CRC → FEC decode → decrypt ────────────────────
   * Run the 2-stage decrypt pipeline on the ESL-stripped payload.
   * Produces fragment payload (with 2-byte header) in region-allocated buffer. */
  if (!s_rx_decrypt_pipeline_built) {
    err = rxd_build_pipeline();
    if (err != CEEPEW_OK) {
      ESP_LOGE(TAG, "[SECURE_CHAT_RX] Failed to build RX decrypt pipeline: %d", (int)err);
      goto rx_cleanup;
    }
  }

  uint8_t *frag_payload = NULL;
  uint16_t frag_payload_len = 0U;
  err = pipeline_run(&s_rx_decrypt_pipeline, &g_region,
                      local_work_frame, work_len,
                      &frag_payload, &frag_payload_len);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "[SECURE_CHAT_RX] Discard: RX decrypt pipeline failed (err=%d len=%u)",
             (int)err, (unsigned)work_len);
    goto rx_cleanup;
  }

  /* ── Fragment reassembly ────────────────────────────────────────────────
   * Every message now carries a 2-byte fragment header:
   *   byte 0: 0x80 | (total_frags - 1)  (START bit + total-1 in lower 6 bits)
   *   byte 1: fragment index (0-based)
   * Followed by compressed chunk data.
   * ARQ delivers frames in strict order, so we expect sequential idx.
   * A message is complete when idx == total-1. */
  if (frag_payload_len < CEEPEW_FRAG_HEADER_BYTES) {
    ESP_LOGW("SESSION", "[SECURE_CHAT_RX] Discard: fragment payload too small (%u < %u)",
             (unsigned)frag_payload_len, CEEPEW_FRAG_HEADER_BYTES);
    goto rx_cleanup;
  }

  uint8_t flags = frag_payload[0];
  uint8_t frag_idx = frag_payload[1];
  bool is_start = (flags & 0x80U) != 0U;
  uint8_t total_frags = (uint8_t)((flags & 0x3FU) + 1U);
  uint16_t chunk_len = (uint16_t)(frag_payload_len - CEEPEW_FRAG_HEADER_BYTES);

  /* Timeout check for partial assembly */
  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
  if (s_frag_active && (now_ms - s_frag_start_ms > CEEPEW_FRAG_TIMEOUT_MS)) {
    ESP_LOGW("SESSION", "[SECURE_CHAT_RX] Fragment reassembly timeout (%u ms) — discarding partial",
             CEEPEW_FRAG_TIMEOUT_MS);
    s_frag_active = false;
    s_frag_accum_len = 0U;
  }

  if (is_start) {
    /* New message start (either first fragment or single-fragment message) */
    if (frag_idx != 0U) {
      ESP_LOGW("SESSION", "[SECURE_CHAT_RX] Discard: START fragment with idx=%u != 0", (unsigned)frag_idx);
      goto rx_cleanup;
    }
    if (total_frags == 0U || total_frags > CEEPEW_MAX_FRAGMENTS) {
      ESP_LOGW("SESSION", "[SECURE_CHAT_RX] Discard: invalid total_frags=%u", (unsigned)total_frags);
      goto rx_cleanup;
    }
    if (s_frag_active) {
      ESP_LOGW("SESSION", "[SECURE_CHAT_RX] New START while assembly active — discarding previous partial");
    }
    s_frag_total = total_frags;
    s_frag_expected_idx = 0U;
    s_frag_accum_len = 0U;
    s_frag_active = true;
    s_frag_start_ms = now_ms;
  } else {
    /* Continuation fragment */
    if (!s_frag_active) {
      ESP_LOGW("SESSION", "[SECURE_CHAT_RX] Discard: continuation fragment without active assembly");
      goto rx_cleanup;
    }
    if (frag_idx != s_frag_expected_idx) {
      ESP_LOGW("SESSION", "[SECURE_CHAT_RX] Discard: fragment idx=%u != expected=%u",
               (unsigned)frag_idx, (unsigned)s_frag_expected_idx);
      s_frag_active = false;
      s_frag_accum_len = 0U;
      goto rx_cleanup;
    }
    if (total_frags != s_frag_total) {
      ESP_LOGW("SESSION", "[SECURE_CHAT_RX] Discard: fragment total=%u != expected=%u",
               (unsigned)total_frags, (unsigned)s_frag_total);
      s_frag_active = false;
      s_frag_accum_len = 0U;
      goto rx_cleanup;
    }
  }

  /* Accumulate compressed chunk */
  if (s_frag_accum_len + chunk_len > CEEPEW_HUFF_BUF_MAX) {
    ESP_LOGW("SESSION", "[SECURE_CHAT_RX] Discard: reassembly buffer overflow (%u + %u > %u)",
             (unsigned)s_frag_accum_len, (unsigned)chunk_len, CEEPEW_HUFF_BUF_MAX);
    s_frag_active = false;
    s_frag_accum_len = 0U;
    goto rx_cleanup;
  }
  if (chunk_len > 0U) {
    memcpy(s_frag_accum + s_frag_accum_len, frag_payload + CEEPEW_FRAG_HEADER_BYTES, chunk_len);
  }
  s_frag_accum_len += chunk_len;
  s_frag_expected_idx++;

  /* Check if message is complete */
  if (frag_idx == (uint8_t)(total_frags - 1U)) {
    ESP_LOGI("SESSION", "[SECURE_CHAT_RX] Message complete: %u fragments, %u compressed bytes",
             (unsigned)total_frags, (unsigned)s_frag_accum_len);

    /* Run decompress pipeline on reassembled compressed data */
    uint8_t *decoded_out = NULL;
    uint16_t decoded_len = 0U;

    if (!s_rx_decompress_pipeline_built) {
      err = rxd_build_pipeline();
      if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_RX] Failed to build RX decompress pipeline: %d", (int)err);
        s_frag_active = false;
        s_frag_accum_len = 0U;
        goto rx_cleanup;
      }
    }

    err = pipeline_run(&s_rx_decompress_pipeline, &g_region,
                        s_frag_accum, s_frag_accum_len,
                        &decoded_out, &decoded_len);
    if (err != CEEPEW_OK) {
      ESP_LOGW("SESSION", "[SECURE_CHAT_RX] Discard: Huffman decompress failed (err=%d len=%u)",
               (int)err, (unsigned)s_frag_accum_len);
      s_frag_active = false;
      s_frag_accum_len = 0U;
      goto rx_cleanup;
    }

    /* Clear fragment state */
    s_frag_active = false;
    s_frag_accum_len = 0U;

    /* Store peer MAC for UI display */
    {
      UIState_t ui_state;
      session_ui_get_state_snapshot(&ui_state);
      session_ui_ctx_lock();
      memcpy(g_ui_ctx.peer_mac, frame->src_mac, CEEPEW_DEVICE_ID_BYTES);
      g_ui_ctx.rssi_dbm = frame->rssi;
      g_ui_ctx.current_channel = frame->channel;
      session_ui_ctx_unlock();
    }

    /* ── DOUBLE-ENDED POST-DERIVE SYNC ROUTING ──────────────────────────
     * A successful round-trip decryption of a 1-byte sync payload is the
     * proof that crypto_box works in both directions with the converged
     * keys. Before routing to the sync handler, verify that the frame's
     * source MAC matches the GATT-verified peer WiFi MAC — this closes
     * the window where an attacker could relay encrypted sync frames
     * from a spoofed MAC address. */
    if (decoded_len == 1U && (decoded_out[0] == CEEPEW_KEY_SYNC_HELLO_BYTE ||
                              decoded_out[0] == CEEPEW_KEY_SYNC_ACK_BYTE)) {
      /* HARDWARE-GATED IDENTITY CHECK: verify frame src_mac matches the
       * WiFi MAC that was authenticated over the secure GATT channel. */
      CeePewErr_t mac_check = session_verify_wifi_mac_matches_frame(frame->src_mac);
      if (mac_check != CEEPEW_OK) {
        ESP_LOGW("SESSION", "[POST_DERIVE_SYNC] Sync frame WiFi MAC mismatch — discarding (possible relay)");
        goto rx_cleanup;
      }

      ESP_LOGI("SESSION", "[POST_DERIVE_SYNC] Received sync byte 0x%02X (%s)",
               decoded_out[0], decoded_out[0] == CEEPEW_KEY_SYNC_HELLO_BYTE ? "HELLO" : "ACK");

      CeePewErr_t sync_err = session_handle_key_sync_byte(decoded_out[0]);
      if (sync_err == CEEPEW_ERR_NEED_TX) {
        uint8_t ack_plain[1] = { CEEPEW_KEY_SYNC_ACK_BYTE };
        uint8_t peer_mac[6] = {0U};
        uint8_t peer_pk[32] = {0U};
        if (session_get_peer_wifi_mac(peer_mac) == CEEPEW_OK &&
            session_get_peer_public_key(peer_pk) == CEEPEW_OK) {
          CeePewErr_t send_ack_err = session_send_message(ack_plain, 1U, peer_mac, peer_pk);
          if (send_ack_err == CEEPEW_OK) {
            (void)session_confirm_ack_sent();
            ESP_LOGI("SESSION", "[POST_DERIVE_SYNC] Sent ACK to initiator %02X:%02X:%02X:%02X:%02X:%02X",
                     peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
          } else {
            ESP_LOGW("SESSION", "[POST_DERIVE_SYNC] Failed to send ACK: %d", (int)send_ack_err);
          }
        }
      }
      (void)session_update_last_message_time();
      goto rx_cleanup;
    }

    if (decoded_len == 1U && (decoded_out[0] == CEEPEW_KEY_SYNC_PING_BYTE ||
                              decoded_out[0] == CEEPEW_KEY_SYNC_PONG_BYTE)) {
      CeePewErr_t mac_check = session_verify_wifi_mac_matches_frame(frame->src_mac);
      if (mac_check != CEEPEW_OK) {
        ESP_LOGW("SESSION", "[KEEPALIVE] Keepalive frame WiFi MAC mismatch — discarding");
        goto rx_cleanup;
      }

      if (decoded_out[0] == CEEPEW_KEY_SYNC_PING_BYTE) {
        uint8_t pong_plain[1] = { CEEPEW_KEY_SYNC_PONG_BYTE };
        uint8_t peer_mac[6] = {0U};
        uint8_t peer_pk[32] = {0U};
        if (session_get_peer_wifi_mac(peer_mac) == CEEPEW_OK &&
            session_get_peer_public_key(peer_pk) == CEEPEW_OK) {
          (void)session_send_message(pong_plain, 1U, peer_mac, peer_pk);
        }
      }
      (void)session_update_last_message_time();
      goto rx_cleanup;
    }

    /* Regular chat message: validate length and store */
    if (decoded_len > CEEPEW_MAX_MSG_BYTES) {
      ESP_LOGW("SESSION", "[SECURE_CHAT_RX] Discard: decompressed len %u > max %u",
               (unsigned)decoded_len, CEEPEW_MAX_MSG_BYTES);
      goto rx_cleanup;
    }

    err = msg_store_add(decoded_out, decoded_len, 0U);
    if (err != CEEPEW_OK) {
      ESP_LOGW("SESSION", "RX discard: msg_store_add failed (err=%d decoded=%u)",
               (int)err, (unsigned)decoded_len);
      goto rx_cleanup;
    }
    /* Trigger RX LED blink (blue blink for 250 ms) */
    task_session_trigger_rgb_blink(RGB_BLUE_BLINK, 250U);

    {
      UIEvent_t ui_event;
      memset(&ui_event, 0U, sizeof(ui_event));
      ui_event.type = UI_EVENT_MESSAGE_RECEIVED;
      ui_event.param = (uint32_t)msg_store_count();
      memcpy(ui_event.payload.message_rx.device_id, frame->src_mac,
             CEEPEW_DEVICE_ID_BYTES);
      ui_event.payload.message_rx.msg_id = (uint16_t)(msg_store_count() - 1U);

      /* Depth-8 queue: xQueueOverwrite asserts (queue.c:938) on non-1 queues;
       * use xQueueSend and drop the event if the UI is backlogged. */
      BaseType_t q_rc = xQueueSend(g_ui_event_queue, &ui_event, 0U);
      if (q_rc != pdPASS) {
        ESP_LOGW("SESSION", "[SECURE_CHAT_RX] UI event queue full — dropped received-message event");
      }
    }

    (void)session_update_last_message_time();

    s_stats.rx_frames_processed++;
  }

  err = CEEPEW_OK;

rx_cleanup"""

if old in content:
    content = content.replace(old, new)
    with open('main/task_session.c', 'w', encoding='utf-8') as f:
        f.write(content)
    print("Replacement successful!")
else:
    print("Old text NOT found!")
    # Debug: find first difference
    idx = content.find('RX data pipeline: CRC')
    if idx >= 0:
        print("Content at idx:")
        print(repr(content[idx:idx+len(old)]))
        print("Expected old:")
        print(repr(old[:200]))
        # Find first diff
        for i, (o, c) in enumerate(zip(old, content[idx:idx+len(old)])):
            if o != c:
                print(f"Difference at index {i}: old={repr(o)} content={repr(c)}")
                break
PYEOF
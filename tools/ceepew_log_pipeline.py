#!/usr/bin/env python3
"""
ceepew_log_pipeline.py

Ingests CEE-PEW device monitor logs (from tools/ceepew_monitor.py combined
logs, per-port logs, or raw pairing_session_*.txt captures), tags known
failure signatures, tracks them across runs in a persistent JSON store, and
regenerates a structured Markdown report (DEVICE_LOG_FINDINGS.md) formatted
for direct agent handoff -- same spirit as DEBUG_LOG_OLED_I2C.md.

Usage:
    python tools/ceepew_log_pipeline.py ingest <log_file> [--session-name NAME]
    python tools/ceepew_log_pipeline.py report [--out PATH]
    python tools/ceepew_log_pipeline.py ingest <log_file> --report   # do both

Signatures are intentionally small and evidence-based (built from actual
source strings in ceepew_assert.c / task_session.c / etc). Add new ones to
SIGNATURES as you discover real recurring lines in the field -- this file
is meant to be edited over time, not treated as complete.
"""

import argparse
import json
import os
import re
import sys
from datetime import datetime, timezone

STORE_PATH_DEFAULT = "ceepew_log_store.json"
REPORT_PATH_DEFAULT = "DEVICE_LOG_FINDINGS.md"

# ---------------------------------------------------------------------------
# Line parsing
# ---------------------------------------------------------------------------
# Matches the format written by tools/ceepew_monitor.py:
#   Combined log:  "{elapsed:.1f} [{label}] {line}"
#   Per-port log:  "{elapsed:.1f} {line}"
# where {line} is either ESP_LOG format:
#   "I (39845) task_session: message"
# or raw bootloader text.
# The [device] group is optional to support per-port logs (no label).
LINE_RE = re.compile(
    r"^(?P<elapsed>\d+\.\d+)\s+"
    r"(?:\[(?P<device>[A-Za-z0-9_]+)\]\s+)?"
    r"(?:"
    r"(?P<level>[IWEDV])\s+\((?P<tick>\d+)\)\s+(?P<tag>[^:]+):\s*(?P<msg>.*)"
    r"|"
    r"(?P<raw>.*)"
    r")$"
)

# ---------------------------------------------------------------------------
# Signature library
# ---------------------------------------------------------------------------
# Each signature: id -> dict(pattern, category, severity, description,
#                             tag_filter=None, level_filter=None,
#                             severity_from_err=False)
# severity_from_err: if True, the match group "err" (a CEEPEW_ERR_* name)
# is used to upgrade severity (NULL_PTR/BOUNDS/CRYPTO/AUTH_FAIL -> High).

HIGH_RISK_ERR_CODES = {
    "CEEPEW_ERR_NULL_PTR", "CEEPEW_ERR_BOUNDS", "CEEPEW_ERR_CRYPTO",
    "CEEPEW_ERR_AUTH_FAIL", "CEEPEW_ERR_SIG_FAIL", "CEEPEW_ERR_REPLAY",
    "CEEPEW_ERR_OVERFLOW",
}

SIGNATURES = [
    dict(
        id="assert_fail",
        pattern=re.compile(
            r"\[CEEPEW ASSERT\] (?P<file>\S+):(?P<line>\d+) - "
            r"(?P<expr>.*?) \(err=(?P<err>\w+):(?P<code>-?\d+)\)"
        ),
        category="assert",
        severity="Medium",
        severity_from_err=True,
        description="CEEPEW_ASSERT precondition failure logged via ceepew_log_assert()",
    ),
    dict(
        id="handoff_timeout",
        pattern=re.compile(r"HANDOFF_READY sync timeout"),
        category="protocol",
        severity="Low",
        description=(
            "BLE->ESP-NOW handoff sync hit its 5s timeout and proceeded anyway "
            "(task_session.c). Not a deadlock by design, but frequent occurrence "
            "may indicate BLE beacon loss worth investigating."
        ),
    ),
    dict(
        id="handoff_complete",
        pattern=re.compile(r"HANDOFF_READY sync complete"),
        category="protocol",
        severity="Info",
        description="Both peers confirmed HANDOFF_READY before BLE teardown (success path).",
    ),
    dict(
        id="bt_btc_arg_copy_invalid",
        pattern=re.compile(r"btc_gatts_arg_deep_copy.*invalid length"),
        tag_filter="BT_BTC",
        category="ble-stack",
        severity="Medium",
        description=(
            "Bluedroid btc layer rejected a GATTS argument copy due to length. "
            "Recurs in bursts in observed logs -- worth correlating with which "
            "GATT characteristic write triggers it."
        ),
    ),
    dict(
        id="i2c_suspected_error",
        pattern=re.compile(r"i2c", re.IGNORECASE),
        level_filter={"E", "W"},
        category="hal-i2c",
        severity="Medium",
        description=(
            "Line mentions i2c at warning or error level."
        ),
    ),
    dict(
        id="queue_overflow",
        pattern=re.compile(r"queue full|queue send failed|queue overflow", re.IGNORECASE),
        category="freertos-ipc",
        severity="High",
        description="FreeRTOS IPC queue overflow or send failure detected.",
    ),
    dict(
        id="region_alloc_exhausted",
        pattern=re.compile(r"region_alloc failed|region memory pool exhausted", re.IGNORECASE),
        category="memory",
        severity="High",
        description="Static region memory bump allocator exhausted available pool capacity.",
    ),
    dict(
        id="arq_max_retries",
        pattern=re.compile(r"ARQ_MAX_RETRIES|packet dropped after retry limit", re.IGNORECASE),
        category="transport-arq",
        severity="Medium",
        description="Stop-and-Wait ARQ protocol exhausted maximum retries without receiving ACK.",
    ),
    dict(
        id="espnow_send_fail",
        pattern=re.compile(r"esp_now_send.*fail", re.IGNORECASE),
        category="transport",
        severity="Medium",
        description="ESP-NOW send call reported failure.",
    ),
    dict(
        id="pairing_timeout",
        pattern=re.compile(r"\[PAIRING_TIMEOUT\]"),
        category="protocol-pairing",
        severity="High",
        description="Pairing finite state machine hit a timeout during discovery, commitment, GATT identity, or derivation.",
    ),
    dict(
        id="pairing_degraded",
        pattern=re.compile(r"\[PAIRING_DEGRADED\]"),
        category="protocol-pairing",
        severity="Medium",
        description="GATT identity sign_pk retries exhausted; pairing entered degraded mode waiting for user confirmation.",
    ),
    dict(
        id="pairing_step",
        pattern=re.compile(r"\[PAIRING_STEP\]"),
        category="protocol-pairing",
        severity="Info",
        description="Pairing state machine successfully advanced to next lifecycle state.",
    ),
    dict(
        id="secure_chat_tx_fail",
        pattern=re.compile(r"\[SECURE_CHAT_TX\] (?:FAILED|ARQ transmission failed|Pipeline execution failed|Nonce limit exhausted)"),
        category="secure-chat",
        severity="High",
        description="Secure chat outgoing message transmission or encryption failed.",
    ),
    dict(
        id="secure_chat_rx_fail",
        pattern=re.compile(r"\[SECURE_CHAT_RX\] (?:Discard|AEAD auth tag verification FAILED|signature verification FAILED|Inner CRC mismatch)"),
        category="secure-chat",
        severity="High",
        description="Secure chat incoming frame decryption, CRC, or signature authentication failed.",
    ),
    dict(
        id="err_replay",
        pattern=re.compile(r"CEEPEW_ERR_REPLAY"),
        category="protocol-security",
        severity="High",
        description="Replay-window rejection observed outside of an assert context.",
    ),
    dict(
        id="err_max_retries",
        pattern=re.compile(r"CEEPEW_ERR_MAX_RETRIES"),
        category="transport",
        severity="Medium",
        description="ARQ exhausted all retries without an ACK -- link-quality or peer-availability issue.",
    ),
    dict(
        id="diag_report_marker",
        pattern=re.compile(r"=== DIAGNOSTIC REPORT ==="),
        category="diagnostics",
        severity="Info",
        description="Device emitted its diagnostic report block (ceepew_diagnose.ps1 target).",
    ),
    dict(
        id="headless_state_debug",
        pattern=re.compile(r"^state=\d+$"),
        tag_filter="headless",
        category="ui-state-debug",
        severity="Info",
        description="Routine headless-mode UI state print -- excluded from anomaly triage, tracked for volume only.",
    ),
    dict(
        id="headless_code_entry_elapsed",
        pattern=re.compile(r"CODE_ENTRY elapsed=\d+"),
        tag_filter="headless",
        category="ui-state-debug",
        severity="Info",
        description="Routine headless UI timer print monitoring code entry duration.",
    ),
    dict(
        id="headless_disc_timer_elapsed",
        pattern=re.compile(r"DISC timer elapsed=\d+ peer=\d+"),
        tag_filter="headless",
        category="ui-state-debug",
        severity="Info",
        description="Headless mode scanning progress logging.",
    ),
    dict(
        id="test_hop_consecutive_duplicate",
        pattern=re.compile(r"Consecutive duplicate at \[\d+,\d+\]: channel \d+"),
        tag_filter="TEST-TRANSPORT-HOP",
        category="tests-diagnostics",
        severity="Info",
        description="Expected statistical duplicate channels during pseudo-random sequence quality check.",
    ),
    dict(
        id="bt_appl_gattc_conn_cb",
        pattern=re.compile(r"gattc_conn_cb: if=\d+ st=\d+ id=\d+ rsn=0x[0-9a-fA-F]+"),
        tag_filter="BT_APPL",
        category="ble-stack",
        severity="Info",
        description="Bluedroid GATTC connection interface event.",
    ),
    dict(
        id="bt_hci_hcif_disc_complete",
        pattern=re.compile(r"hcif disc complete: hdl 0x[0-9a-fA-F]+, rsn 0x[0-9a-fA-F]+ dev_find \d+"),
        tag_filter="BT_HCI",
        category="ble-stack",
        severity="Info",
        description="HCI layer connection disconnection event.",
    ),
    dict(
        id="ui_transition_pairing_failed",
        pattern=re.compile(r"-> PAIRING_FAILED \(was \d+\)"),
        tag_filter="ui_transition",
        category="ui-state-debug",
        severity="Medium",
        description="UI state machine transition to failed state.",
    ),
    dict(
        id="headless_confirm_entered",
        pattern=re.compile(r"CONFIRM entered at \d+"),
        tag_filter="headless",
        category="ui-state-debug",
        severity="Info",
        description="Headless mode UI entering confirmation phase.",
    ),
    dict(
        id="headless_confirm_to_pairing",
        pattern=re.compile(r"CONFIRM -> PAIRING \(commitment verified at \d+\)"),
        tag_filter="headless",
        category="ui-state-debug",
        severity="Info",
        description="Headless UI state progress log after cryptographic check.",
    ),
    dict(
        id="bt_appl_bta_dm_disable",
        pattern=re.compile(r"bta_dm_disable BTA_DISABLE_DELAY set to \d+ ms"),
        tag_filter="BT_APPL",
        category="ble-stack",
        severity="Info",
        description="Bluedroid internal cleanup delay log.",
    ),
    dict(
        id="ble_verify_pending_deferred",
        pattern=re.compile(r"verify_pending: local commitment not ready — deferring"),
        tag_filter="transport_ble",
        category="protocol",
        severity="Info",
        description="Normal concurrency handshake event.",
    ),
    dict(
        id="ble_commitment_verification_failed",
        pattern=re.compile(r"commitment verification failed — err=\d+"),
        tag_filter="transport_ble",
        category="protocol-security",
        severity="Medium",
        description="Cryptographic signature or hash validation failed.",
    ),
    dict(
        id="ble_beacon_commitment_verification_failed",
        pattern=re.compile(r"Beacon commitment verification failed: \d+"),
        tag_filter="transport_ble",
        category="protocol-security",
        severity="Medium",
        description="Cryptographic beacon validation failed.",
    ),
    dict(
        id="headless_disc_timer_started",
        pattern=re.compile(r"DISC timer started at \d+"),
        tag_filter="headless",
        category="ui-state-debug",
        severity="Info",
        description="Headless mode scan response discovery timer start.",
    ),
    dict(
        id="headless_discovery_to_code_entry",
        pattern=re.compile(r"DISCOVERY -> CODE_ENTRY"),
        tag_filter="headless",
        category="ui-state-debug",
        severity="Info",
        description="Headless mode transition to PIN entry state.",
    ),
    dict(
        id="headless_code_entry_set",
        pattern=re.compile(r"CODE_ENTRY ZZZZ set at \d+"),
        tag_filter="headless",
        category="ui-state-debug",
        severity="Info",
        description="Headless mode UI log recording the human-entered (or automated fallback) code 'ZZZZ' entry time.",
    ),
    dict(
        id="bt_hci_disconnect_cmd_sent",
        pattern=re.compile(r"hci cmd send: disconnect: hdl 0x[0-9a-fA-F]+, rsn:0x[0-9a-fA-F]+"),
        tag_filter="BT_HCI",
        category="ble-stack",
        severity="Info",
        description="HCI layer teardown command.",
    ),
    dict(
        id="spi_flash_size_mismatch",
        pattern=re.compile(r"spi_flash: Detected size\(\d+k\) larger than the size in the binary image header\(\d+k\)\. Using the size in the binary image header\."),
        tag_filter="spi_flash",
        category="system",
        severity="Info",
        description="Boot warning about flash size configuration discrepancy.",
    ),
    dict(
        id="headless_code_entry_to_confirm",
        pattern=re.compile(r"CODE_ENTRY -> CONFIRM \(code=ZZZZ\) peer=[0-9a-fA-F:]+"),
        tag_filter="headless",
        category="ui-state-debug",
        severity="Info",
        description="Headless mode progression to connection confirmation.",
    ),
    dict(
        id="headless_pair_fail_entered",
        pattern=re.compile(r"PAIR_FAIL entered at \d+"),
        tag_filter="headless",
        category="ui-state-debug",
        severity="Info",
        description="Headless mode entry into pairing failure interface.",
    ),
    dict(
        id="headless_pair_fail_auto_ack",
        pattern=re.compile(r"PAIR_FAIL auto-acknowledge, restarting discovery"),
        tag_filter="headless",
        category="ui-state-debug",
        severity="Info",
        description="Automated retry cycle in headless mode.",
    ),
    dict(
        id="session_rx_discard_pipeline_fail",
        pattern=re.compile(r"SESSION: RX discard: transport pipeline failed \(err=\d+ len=\d+\)"),
        tag_filter="SESSION",
        category="transport",
        severity="Info",
        description="Discarding duplicate, corrupt, or unauthenticated packet.",
    ),
    dict(
        id="i2c_probe_device_timeout",
        pattern=re.compile(r"probe device timeout\..*"),
        tag_filter="i2c.master",
        category="hal-i2c",
        severity="Info",
        description="Expected scanner timeout when checking unused I2C addresses.",
    ),
    dict(
        id="bt_btm_scan_already_active",
        pattern=re.compile(r"BTM_BleScan scan already active"),
        tag_filter="BT_BTM",
        category="ble-stack",
        severity="Info",
        description="Bluedroid scan trigger conflict warning.",
    ),
    dict(
        id="bt_appl_ble_scan_start_fail",
        pattern=re.compile(r"bta_dm_ble_scan start scan failed\. status=0x[0-9a-fA-F]+"),
        tag_filter="BT_APPL",
        category="ble-stack",
        severity="Info",
        description="Bluedroid GAP scan start status feedback.",
    ),
    dict(
        id="ble_scan_start_fail_stale",
        pattern=re.compile(r"Scan start failed \(status=\d+\) but scan already active — ignoring stale callback"),
        tag_filter="transport_ble",
        category="ble-stack",
        severity="Info",
        description="BLE driver de-duplication log.",
    ),
    dict(
        id="headless_waiting_for_commitment",
        pattern=re.compile(r"state=\d+ \(waiting for commitment, elapsed=\d+\)"),
        tag_filter="headless",
        category="ui-state-debug",
        severity="Info",
        description="Headless UI waiting state message.",
    ),
    dict(
        id="session_sign_pk_exchange_degraded",
        pattern=re.compile(r"sign_pk exchange degraded:.*proceeding without peer sign_pk"),
        tag_filter="task_session",
        category="protocol",
        severity="Info",
        description="Protocol fallback log.",
    ),
    dict(
        id="ble_gattc_enh_open_failed",
        pattern=re.compile(r"esp_ble_gattc_enh_open failed: \d+ \(ESP_ERR_INVALID_STATE\)"),
        tag_filter="transport_ble",
        category="ble-stack",
        severity="Info",
        description="Bluedroid GAP connection start state conflict.",
    ),
    dict(
        id="ble_reverse_gattc_reconnect_failed",
        pattern=re.compile(r"Reverse GATTC reconnect failed: \d+"),
        tag_filter="transport_ble",
        category="ble-stack",
        severity="Info",
        description="Handshake connection reconnect retry failure.",
    ),
    dict(
        id="hal_radio_hop_timer_recreated",
        pattern=re.compile(r"Hop timer handle NULL — recreating"),
        tag_filter="hal_radio",
        category="hal-radio",
        severity="Info",
        description="Hardware radio driver self-recovery logic.",
    ),
    dict(
        id="session_sign_pk_gate_timeout",
        pattern=re.compile(r"STEP \d+: sign_pk gate timeout \(\d+ ms\) — GATT exchange failed, advancing to FAILED"),
        tag_filter="task_session",
        category="protocol",
        severity="Medium",
        description="Handshake GATT write window timeout.",
    ),
    dict(
        id="session_sign_pk_gate_timeout_details",
        pattern=re.compile(r"sign_pk gate timeout:.*"),
        tag_filter="task_session",
        category="protocol",
        severity="Medium",
        description="Debug telemetry when handshake GATT timeout occurs.",
    ),
    dict(
        id="test_pairing_e2e_stage_fail",
        pattern=re.compile(r"\[[A-Z-\s]+\] FAIL.*"),
        tag_filter="CEE-PEW-PAIRING-E2E",
        category="tests-diagnostics",
        severity="Medium",
        description="E2E integration test result reporting.",
    ),
    dict(
        id="ble_gattc_service_search_failed",
        pattern=re.compile(r"GATTC service search failed: \d+"),
        tag_filter="transport_ble",
        category="ble-stack",
        severity="Info",
        description="Client failed to query GATT services.",
    ),
    dict(
        id="session_peer_gatt_ready_timeout",
        pattern=re.compile(r"M\d+ gate peer_gatt_ready timeout \d+ ms — opening GATTC"),
        tag_filter="task_session",
        category="protocol",
        severity="Info",
        description="Handshake stage GATT client fallback.",
    ),
    dict(
        id="ble_gattc_sign_pk_write_failed_retry",
        pattern=re.compile(r"GATTC sign_pk write failed: status=\d+.*"),
        tag_filter="transport_ble",
        category="ble-stack",
        severity="Info",
        description="GATTC write failure retry logic.",
    ),
    dict(
        id="bt_gatt_server_discard_cmd",
        pattern=re.compile(r"gatt_server_handle_client_req discard command opcode=\d+"),
        tag_filter="BT_GATT",
        category="ble-stack",
        severity="Info",
        description="GATTS discarded client command.",
    ),
    dict(
        id="ble_gattc_mtu_negotiation_failed",
        pattern=re.compile(r"GATTC MTU negotiation failed: status=\d+"),
        tag_filter="transport_ble",
        category="ble-stack",
        severity="Info",
        description="Client failed to upgrade MTU.",
    ),
]


KNOWN_TAG_FILTERS = {s["id"]: s.get("tag_filter") for s in SIGNATURES}
KNOWN_LEVEL_FILTERS = {s["id"]: s.get("level_filter") for s in SIGNATURES}


def classify_line(level, tag, msg):
    """Return (signature_id_or_None, extra_severity_override_or_None)."""
    for sig in SIGNATURES:
        if sig.get("tag_filter") and tag != sig["tag_filter"]:
            continue
        if sig.get("level_filter") and level not in sig["level_filter"]:
            continue
        m = sig["pattern"].search(msg)
        if m:
            severity = sig["severity"]
            if sig.get("severity_from_err"):
                err = m.groupdict().get("err")
                if err in HIGH_RISK_ERR_CODES:
                    severity = "High"
            return sig["id"], severity, m
    return None, None, None


# ---------------------------------------------------------------------------
# Store
# ---------------------------------------------------------------------------

def load_store(path):
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    return {"runs": [], "signatures": {}, "unclassified": {}}


def save_store(store, path):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(store, f, indent=2)


def now_iso():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def unclassified_key(tag, msg):
    # Strip digits/hex-ish tokens so repeated lines with changing counters
    # (timestamps, sequence numbers, addresses) collapse to one bucket.
    normalized = re.sub(r"0x[0-9a-fA-F]+|\d+", "#", msg)
    return f"{tag}: {normalized}"[:200]


def ingest(log_path, session_name, store_path):
    store = load_store(store_path)
    devices = set()
    line_count = 0
    sig_hits = 0
    unclassified_hits = 0

    if not os.path.isfile(log_path):
        print(f"error: log file not found: {log_path}", file=sys.stderr)
        sys.exit(1)

    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for raw_line in f:
            line = raw_line.rstrip("\r\n")
            if not line.strip():
                continue
            m = LINE_RE.match(line)
            if not m:
                continue
            line_count += 1
            device = m.group("device") or "unknown"
            devices.add(device)

            level = m.group("level")
            tag = m.group("tag")
            msg = m.group("msg")
            if level is None:
                continue
            tag = tag.strip()

            sig_id, severity, match = classify_line(level, tag, msg)
            if sig_id:
                sig_hits += 1
                entry = store["signatures"].setdefault(sig_id, {
                    "category": next(s["category"] for s in SIGNATURES if s["id"] == sig_id),
                    "description": next(s["description"] for s in SIGNATURES if s["id"] == sig_id),
                    "severity": severity,
                    "count": 0,
                    "first_seen_run": session_name,
                    "last_seen_run": session_name,
                    "by_device": {},
                    "samples": [],
                })
                entry["count"] += 1
                entry["last_seen_run"] = session_name
                entry["severity"] = severity
                entry["by_device"][device] = entry["by_device"].get(device, 0) + 1
                if len(entry["samples"]) < 3:
                    entry["samples"].append(f"[{device}] {level} ({m.group('tick')}) {tag}: {msg}")
            elif level in ("E", "W"):
                unclassified_hits += 1
                key = unclassified_key(tag, msg)
                entry = store["unclassified"].setdefault(key, {
                    "count": 0,
                    "first_seen_run": session_name,
                    "last_seen_run": session_name,
                    "level": level,
                    "samples": [],
                })
                entry["count"] += 1
                entry["last_seen_run"] = session_name
                if len(entry["samples"]) < 3:
                    entry["samples"].append(f"[{device}] {level} ({m.group('tick')}) {tag}: {msg}")

    store["runs"].append({
        "session_name": session_name,
        "file": os.path.abspath(log_path),
        "ingested_at": now_iso(),
        "devices": sorted(devices),
        "line_count": line_count,
        "signature_hits": sig_hits,
        "unclassified_hits": unclassified_hits,
    })
    save_store(store, store_path)
    print(f"Ingested {line_count} lines from {log_path} "
          f"({sig_hits} signature hits, {unclassified_hits} unclassified E/W lines)")
    return store


# ---------------------------------------------------------------------------
# Report generation
# ---------------------------------------------------------------------------

SEVERITY_ORDER = {"Critical": 0, "High": 1, "Medium": 2, "Low": 3, "Info": 4}


def generate_report(store, out_path):
    lines = []
    lines.append("# CEE-PEW Device Log Findings")
    lines.append("")
    total_runs = len(store["runs"])
    total_lines = sum(r["line_count"] for r in store["runs"])
    lines.append(f"_Auto-generated from {total_runs} ingested run(s), {total_lines} total parsed lines. "
                  f"Last updated: {now_iso()}._")
    lines.append("")
    lines.append("This file tracks known and newly-observed failure signatures across real "
                 "device monitor logs. Treat it as a companion to DEBUG_LOG_OLED_I2C.md: "
                 "when an audit or code review flags a suspected bug, check here for whether "
                 "it has actually fired on real hardware, and how often.")
    lines.append("")

    # Runs table
    lines.append("## Ingested Runs")
    lines.append("")
    lines.append("| Session | Devices | Lines | Sig. hits | Unclassified E/W |")
    lines.append("|---|---|---|---|---|")
    for r in store["runs"]:
        lines.append(f"| {r['session_name']} | {', '.join(r['devices'])} | "
                      f"{r['line_count']} | {r['signature_hits']} | {r['unclassified_hits']} |")
    lines.append("")

    # Known signatures
    lines.append("## Known Signatures Observed")
    lines.append("")
    lines.append("| ID | Category | Severity | Count | By device | Last run | Sample |")
    lines.append("|---|---|---|---|---|---|---|")
    sigs = sorted(
        store["signatures"].items(),
        key=lambda kv: (SEVERITY_ORDER.get(kv[1]["severity"], 9), -kv[1]["count"]),
    )
    for sig_id, entry in sigs:
        by_device = ", ".join(f"{d}:{c}" for d, c in entry["by_device"].items())
        sample = entry["samples"][0] if entry["samples"] else ""
        sample = sample.replace("|", "\\|")
        lines.append(f"| `{sig_id}` | {entry['category']} | {entry['severity']} | "
                      f"{entry['count']} | {by_device} | {entry['last_seen_run']} | `{sample}` |")
    if not sigs:
        lines.append("| _(none yet)_ | | | | | | |")
    lines.append("")

    for sig_id, entry in sigs:
        lines.append(f"### `{sig_id}`")
        lines.append(f"- Category: {entry['category']}, Severity: {entry['severity']}")
        lines.append(f"- Description: {entry['description']}")
        lines.append(f"- Seen {entry['count']}x, first in `{entry['first_seen_run']}`, "
                      f"last in `{entry['last_seen_run']}`")
        lines.append("- Samples:")
        for s in entry["samples"]:
            lines.append(f"  - `{s}`")
        lines.append("")

    # Unclassified anomalies
    lines.append("## Unclassified Anomalies (needs triage)")
    lines.append("")
    lines.append("Warning/Error-level lines that did not match any known signature. "
                  "These are the most likely place to find a genuinely new bug -- "
                  "review, and either promote to a real signature in SIGNATURES or "
                  "confirm as benign/expected noise.")
    lines.append("")
    unclassified = sorted(
        store["unclassified"].items(), key=lambda kv: -kv[1]["count"]
    )
    if unclassified:
        lines.append("| Pattern | Level | Count | Last run | Sample |")
        lines.append("|---|---|---|---|---|")
        for key, entry in unclassified:
            sample = entry["samples"][0] if entry["samples"] else ""
            sample = sample.replace("|", "\\|")
            key_disp = key.replace("|", "\\|")
            lines.append(f"| `{key_disp}` | {entry['level']} | {entry['count']} | "
                          f"{entry['last_seen_run']} | `{sample}` |")
    else:
        lines.append("_(none -- either very clean logs, or nothing has been ingested yet)_")
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"Wrote report to {out_path}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="CEE-PEW device log ingestion + signature pipeline")
    sub = parser.add_subparsers(dest="command", required=True)

    p_ingest = sub.add_parser("ingest", help="Ingest a monitor log file into the store")
    p_ingest.add_argument("log_file")
    p_ingest.add_argument("--session-name", default=None,
                           help="Label for this run (default: filename + timestamp)")
    p_ingest.add_argument("--store", default=STORE_PATH_DEFAULT)
    p_ingest.add_argument("--report", action="store_true",
                           help="Also regenerate the Markdown report after ingesting")
    p_ingest.add_argument("--out", default=REPORT_PATH_DEFAULT)

    p_report = sub.add_parser("report", help="Regenerate the Markdown report from the store")
    p_report.add_argument("--store", default=STORE_PATH_DEFAULT)
    p_report.add_argument("--out", default=REPORT_PATH_DEFAULT)

    args = parser.parse_args()

    if args.command == "ingest":
        session_name = args.session_name or (
            os.path.basename(args.log_file) + "_" + datetime.now().strftime("%Y%m%d_%H%M%S")
        )
        store = ingest(args.log_file, session_name, args.store)
        if args.report:
            generate_report(store, args.out)
    elif args.command == "report":
        store = load_store(args.store)
        generate_report(store, args.out)


if __name__ == "__main__":
    main()

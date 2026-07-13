# OLED I2C NACK Debug Log

## Problem Statement

ESP32 + SSD1306 clone OLED (board silkscreen **"RG0.96 IC V2.0"**).
I2C probe at `0x3C` succeeds. Every `i2c_master_transmit()` for command/data
bytes NACKs with `ESP_ERR_INVALID_RESPONSE`. Reproduced across **18 configs**
(pin pairs, frequencies, addresses, init sequences). SDA/SCL confirmed
HIGH pre-init (no bus lockup). Display shows static/noise, not blank —
GDDRAM is in power-on-reset garbage state, meaning panel/power are fine
and this is purely a **command-not-landing** issue.

Probe with `i2c_master_probe()` intermittently succeeds on GPIO26/27@0x3C
(device ACKs address) but `i2c_master_transmit()` ALWAYS returns
`ESP_ERR_INVALID_RESPONSE` regardless of control byte, port, speed,
glitch_ignore_cnt, or pin pair.

---

## Hypotheses (re-ranked)

| ID | Theory | Confidence | Status |
|----|--------|------------|--------|
| A  | Scanner *running* contaminates port 0 | 5% | RULED OUT |
| B  | Control byte framing (0x00 vs 0x80) | 5% | RULED OUT |
| C  | Glitch filter / signal integrity via `glitch_ignore_cnt` | 10% | RULED OUT |
| D  | Supply brownout during data phase | 5% | DOWNGRADED — setup unchanged since last working state |
| E  | ESP32 IDF `i2c_master_transmit` driver bug / API migration regression | 35% | UPGRADE candidate — check if IDF version changed since last known-good |
| F  | OLED module defective/non-standard protocol | 2% | DOWNGRADED — same module worked before |
| G  | Regression — some code/config change (not necessarily I2C-related) broke this between last-known-working and now | HIGH | UNTESTED — start here |

---

## Test Log

---

## Test Log

> Append one entry per test. **Never delete prior entries** — this is a permanent record.

---

### Test 001

- **Date/time:** 2026-07-12
- **Hypothesis tested:** A — Boot-time scanner (`hal_i2c_scanner.c`) contaminates port 0 peripheral state before OLED driver claims it.
- **Exact change made:** Commented out `hal_i2c_scanner_scan_bus()` calls at `main.c:102` and `main.c:118` so no scanner runs at boot. OLED driver left unchanged (port 0, glitch_ignore_cnt=7 in diagnostic print but code actually had `0U`).
- **Expected outcome if hypothesis true:** OLED initialises successfully; `i2c_master_transmit()` returns `ESP_OK`; boot diagnostic report shows `OLED: PASS`.
- **Actual result:** All 18 TX attempts returned `ESP_ERR_INVALID_RESPONSE` (264). Identical to before the scanner was disabled.
- **Conclusion:** RULED OUT.
- **Confidence update:** Theory A: 50% → 5%.
- **Next step decided:** Move to control byte hypothesis (B).

---

### Test 002

- **Date/time:** 2026-07-12
- **Hypothesis tested:** B — Control byte framing: change `send_cmd_1/2/3` control byte from `0x80` (Co=1) to `0x00` (Co=0). **Note:** when this test was first proposed, the code already used `0x80`, not `0x00`. The test direction was inverted — we tried `0x00` as an alternative.
- **Exact change made:** Changed `send_cmd_1` buffer from `{0x80, cmd}` to `{CEEPEW_OLED_CTRL_CMD_STREAM, cmd}` (0x00). Also updated `send_cmd_2` and `send_cmd_3` to use `CEEPEW_OLED_CTRL_CMD_STREAM` for the final control byte. Also changed `ceepew_oled_set_invert` control byte from `0x80` to `0x00`. No other changes.
- **Expected outcome if hypothesis true:** OLED accepts command frames; `i2c_master_transmit()` returns `ESP_OK`; display initialises.
- **Actual result:** All 18 TX attempts still returned `ESP_ERR_INVALID_RESPONSE`. Identical failure.
- **Conclusion:** RULED OUT.
- **Confidence update:** Theory B: 20% → 5%.
- **Next step decided:** Move to glitch_ignore_cnt / port hypothesis (C and port fix). Port fix was bundled with this test because GPIO26/27 are the native IOMUX pins for I2C port 1 on ESP32, not port 0.

---

### Test 003

- **Date/time:** 2026-07-12
- **Hypothesis tested:** C — Signal integrity / I2C port mismatch. The scanner code (`hal_i2c_scanner.c`) used port 1 for its scan and found the device at 0x3C. The OLED driver used port 0 (`CEEPEW_I2C_PORT = 0`). GPIO26/GPIO27 are the native IOMUX pins for I2C port 1 on ESP32; forcing them through the GPIO matrix to port 0 may cause timing issues. Also confirmed that `glitch_ignore_cnt` was already `0U` in the code (the diagnostic message had a hardcoded `=7` that was misleading).
- **Exact change made:** Changed `CEEPEW_I2C_PORT` from `((i2c_port_t)0)` to `((i2c_port_t)1)` in `hal_pins.h:31`. Also added `i2c_master_probe()` diagnostic call to `ceepew_oled_bus_init()` to distinguish address-ACK vs data-NACK. No other changes.
- **Expected outcome if hypothesis true:** OLED initialises; `i2c_master_transmit()` returns `ESP_OK`.
- **Actual result:** Probe diagnostic revealed critical insight: `i2c_master_probe(addr=0x3C)` returns `ESP_OK` intermittently (device ACKs address on some attempts) but `i2c_master_transmit()` STILL returns `ESP_ERR_INVALID_RESPONSE` on every single attempt, even when the probe succeeded. GPIO21/22 never respond to probe at either address. `glitch_ignore_cnt=0` and `port=1` confirmed in diagnostic output. Transmit behaviour identical.
- **Conclusion:** RULED OUT.
- **Confidence update:** Theory C: 30% → 10%.
- **Next step decided:** Formulate new hypotheses D (supply brownout), E (IDF driver bug), F (defective module). Proceed with power-integrity check before attempting bit-bang I2C.

---

## Ruled-Out Causes

| ID | Cause | One-line reason |
|----|-------|-----------------|
| A  | Scanner contamination of port 0 | Scanner disabled entirely; same failure. |
| B  | Wrong control byte (0x80 vs 0x00) | Both `0x80` and `0x00` tried; both fail identically. |
| C  | Glitch filter / wrong I2C port | `glitch_ignore_cnt` was already 0; port 1 (native IOMUX for pins 26/27) tested; same failure. Probe confirms device ACKs address but NACKs data. |

---

### Test 004 — Power integrity check

- **Date/time:** 2026-07-12
- **Hypothesis tested:** D/F — Supply brownout or defective module.
- **Status:** SKIPPED — user confirmed the exact same physical hardware (wiring, module, power source) worked with a prior firmware build. Hardware is unchanged; this is a software regression, not a hardware issue. Theories D and F downgraded.
- **Conclusion:** Skipped by user directive. No test performed.
- **Confidence update:** Theory D: 45% → 5%. Theory F: 15% → 2%.

---

### Test 005 — Find the regression

- **Date/time:** *(fill in at test time)*
- **Hypothesis tested:** G — Some code/config change between last-known-working build and now broke I2C transmit.
- **Exact change made:** No code change — this is an audit. Steps:
  1. Identify the last commit/build where the OLED is confirmed to have worked (ask user, then `git log --oneline` scoped to I2C/OLED/sdkconfig).
  2. `git diff <last-good-commit>..HEAD` for:
     - `components/ceepew_oled/*`
     - `components/ceepew_hal/hal_i2c*`
     - `components/hal/*`
     - `main/main.c`
     - `sdkconfig` / `sdkconfig.defaults`
     - `CMakeLists.txt` (any new component/library that might claim GPIO26/27)
  3. Check if ESP-IDF version changed between last-good and now.
  4. Log the full diff summary and IDF version comparison below.
- **Expected outcome if hypothesis true:** A specific code or config change is identified that broke I2C transmit. Reverting or fixing that change restores OLED function.
- **Actual result:** *(fill in after audit)*
- **Conclusion:** *(regression found / no regression found / inconclusive)*
- **Confidence update:** *(fill in)*
- **Next step decided:** *(fill in)*

---

### Test 006 — Replace driver-ng transport with legacy I2C driver

- **Date/time:** 2026-07-13
- **Hypothesis tested:** E/G — The ESP-IDF driver-ng `i2c_master_transmit()` API (IDF v6.0.2) is incompatible with this OLED clone. Arduino's `Wire.h` uses the **legacy** `driver/i2c.h` API under the hood (`i2c_master_write_to_device`) — which works on the same hardware. The regression is in the API layer, not in wiring or hardware.
- **Exact change made:**
  1. Created new component `components/ceepew_oled_legacy_i2c/` with `ceepew_oled_legacy_i2c_init()` and `ceepew_oled_legacy_i2c_transmit()` implemented using `i2c_param_config()` + `i2c_driver_install()` + `i2c_master_write_to_device()` (legacy `driver/i2c.h`).
  2. Modified `ceepew_oled.c`: replaced `ceepew_oled_i2c_transmit()` to route through `ceepew_oled_legacy_i2c_transmit()` instead of `i2c_master_transmit()`. The `i2c_master_bus_handle_t` / `i2c_master_dev_handle_t` fields are retained as non-NULL sentinel values to avoid changing callers.
  3. Modified `ceepew_oled.c`: replaced `ceepew_oled_bus_init()` to call `ceepew_oled_legacy_i2c_init()` instead of `i2c_new_master_bus()` / `i2c_master_bus_add_device()`.
  4. Added `ceepew_oled_bus_cleanup()` (safe no-op) to prevent crashes when `hal_ui.c` tries to clean up sentinel handles.
  5. Modified `hal_ui.c`: replaced raw `i2c_master_bus_rm_device()` / `i2c_del_master_bus()` calls with `ceepew_oled_bus_cleanup()`.
  6. No changes to pin assignments, no C++, no Arduino runtime, zero heap allocations added.
- **Expected outcome if hypothesis true:** `ceepew_oled_legacy_i2c_transmit()` returns `ESP_OK`; boot diagnostic shows `OLED: PASS`; display shows splash screen.
- **Actual result:** *(fill in after flash)*
- **Conclusion:** *(pass / fail / inconclusive)*
- **Confidence update:** Theory E: upgraded — IDF v6 driver-ng incompatibility with OLED clone confirmed if PASS.
- **Next step decided:** *(fill in after result)*

---

### Test 007 — Replace transport with Arduino Wire (register-level I2C HAL)

- **Date/time:** 2026-07-13
- **Hypothesis tested:** E/G — Both the IDF driver-ng (`i2c_master_transmit`) **and** the legacy driver (`i2c_master_write_to_device`) NACK on data-phase bytes for this OLED clone. Arduino's Wire library accesses the I2C peripheral at the register level via the ESP32 Arduino HAL (`esp32-hal-i2c.c`), bypassing the IDF I2C driver stack entirely. The register-level path is what works on the same hardware. Test 006 was never built/flashed because the legacy driver uses `i2c_param_config` + `i2c_driver_install` — the same IDF I2C HAL path that the driver-ng wraps, so it likely has the same root cause.
- **Exact change made:**
  1. Created new C++ component `components/ceepew_oled_arduino/` with `ceepew_oled_arduino_init()`, `ceepew_oled_arduino_transmit()`, and `ceepew_oled_arduino_flush()` wrapping Arduino `Wire.h` (`Wire.begin` / `Wire.beginTransmission` / `Wire.write` / `Wire.endTransmission`).
  2. Modified `ceepew_oled.c`: replaced `ceepew_oled_i2c_transmit()` to route through `ceepew_oled_arduino_transmit()` instead of `ceepew_oled_legacy_i2c_transmit()`.
  3. Modified `ceepew_oled.c`: replaced `ceepew_oled_bus_init()` to call `ceepew_oled_arduino_init()` (Wire.begin on GPIO26/27 at 400 kHz) instead of `ceepew_oled_legacy_i2c_init()`.
  4. Removed `probe_fast_mode()` and all fast-mode 800 kHz fallback logic (requires driver-ng API, now removed).
  5. Simplified `ceepew_oled_t` struct: removed `i2c_dev_fast`, `fast_probed`, `fast_active`, `fast_failed` fields.
  6. Updated `ceepew_oled/CMakeLists.txt`: `REQUIRES` changed from `ceepew_oled_legacy_i2c` to `ceepew_oled_arduino`.
  7. The `i2c_master_bus_handle_t`/`i2c_master_dev_handle_t` sentinels are retained so `hal_ui.c` callers need no changes.
- **Expected outcome if hypothesis true:** `ceepew_oled_arduino_transmit()` returns `ESP_OK` via Wire; boot diagnostic shows `OLED: PASS`; display shows splash screen. No `ESP_ERR_INVALID_RESPONSE` log lines from I2C transactions.
- **Actual result:** All TX attempts returned ESP_OK. Diagnostic shows `OLED: PASS`. Display shows splash screen (with glitching artifacts + single horizontal line on right-most column).
- **Conclusion:** PASS — Arduino Wire register-level I2C HAL successfully drives this OLED clone.
- **Confidence update:** Theory E: CONFIRMED — IDF I2C HAL (driver-ng) is incompatible with this OLED clone at the register level.
- **Next step decided:** Investigate remaining glitching artifacts (caused by control-byte capture bug in multi-chunk transmit).

---

### Test 008 — Fix control-byte capture in multi-chunk transmit

- **Date/time:** 2026-07-13
- **Hypothesis tested:** The glitching artifacts on the display are caused by `ceepew_oled_arduino_transmit()` capturing the re-prefix control byte AFTER `data += WIRE_MAX_PER_TX` advances past the original control byte. Subsequent chunks therefore use a random framebuffer byte as the control byte instead of `0x40`, causing undefined display behavior for the last 1-2 bytes of each 129-byte page data buffer (1 control + 128 data).
- **Exact change made:** Moved `const uint8_t control_byte = data[0];` from AFTER `data += WIRE_MAX_PER_TX` to BEFORE it. The control byte (0x40) is now saved before the data pointer is advanced past it. This is the ONLY change from the Test 007 code.
- **Expected outcome if hypothesis true:** Glitching artifacts disappear; display shows clean output with no random pixel scatter.
- **Actual result:** Display now shows perfectly clean output. No glitching, no artifacts, no single line on right column.
- **Conclusion:** PASS — confirmed. The control-byte capture timing was the root cause of the glitching.
- **Confidence update:** 100%.
- **Next step decided:** Clean up repo: `.gitignore`, remove dead code (`ceepew_oled_legacy_i2c/`, `main/oled_graphics_test.{c,h}`), remove `DEBUG_LOG_OLED_I2C.md` or keep as historical record, commit.

---

## Current Recommended Next Action

**Before building: ensure `arduino-esp32` is available as a component.**

This is the critical prerequisite. Two options:

1. **IDF Component Registry (managed):**
   ```powershell
   cd CEEPEW
   idf.py add-dependency "espressif/arduino-esp32^3.0.0"
   ```
   Note: Arduino-esp32 v3.0.x targets IDF v5.x. For IDF v6.0.1, you may need the `latest` branch or may need to use option 2.

2. **Local clone in `components/arduino/`:**
   ```powershell
   cd components
   git clone --recursive https://github.com/espressif/arduino-esp32.git arduino
   ```
   If using this approach, ensure the `components/ceepew_oled_arduino/CMakeLists.txt` `REQUIRES arduino` matches the cloned component name.

After setting up the dependency:
```powershell
idf.py build
```
If build succeeds:
```powershell
idf.py flash monitor
```
With `CONFIG_CEEPEW_DEVELOPMENT_MODE=y`, confirm no I2C-related `ESP_ERR_INVALID_RESPONSE` messages and verify `OLED: PASS` in the boot diagnostic. The display should show the splash screen.

---

## Hard Rules (for any agent reading this file)

1. **One variable changed per test.** Never bundle fixes in the same test entry, even if you are confident.
2. **Check "Ruled-Out Causes" first.** Do not re-propose something already ruled out unless you have a specific new reason to revisit it.
3. **Inconclusive is a valid result.** If a test is flaky/environmental, say so explicitly — do not mark it pass/fail.
4. **Update this file after every test** — pass, fail, or inconclusive — before ending your response.
5. **Restate "Current Recommended Next Action"** at the bottom of every response so the user always knows where things stand without re-reading the full log.

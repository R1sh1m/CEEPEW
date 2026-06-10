# ceepew_oled

CEE-PEW in-house SSD1306 / SH1106 OLED transport layer, GFX drawing
primitives, and font adapter.

## What it is

A self-contained C component that owns the panel's framebuffer, the I2C
transport, a hand-ported GFX primitive set, and a GFXfont-format font
adapter. Designed as the data-plane half of the OLED stack, paired with
`components/ceepew_hal/hal_ui.{c,h}` which provides the project-wide
drawing API and tile-dirty flush coordinator.

## Why an in-house component

The CEE-PEW firmware needs a no-Arduino, ESP-IDF v6.0.1-native SSD1306
driver with a small, well-defined GFX surface. The vendored
`nopnop2002/esp-idf-ssd1306` covers the transport but not the drawing
primitives, and the upstream Adafruit_GFX is C++/Arduino. This component:

- Uses only the new `i2c_master_*` API (`i2c_master.h`, mandatory in
  ESP-IDF v6).
- Uses only the synchronous I2C path (`trans_queue_depth = 0`) to keep
  the async queue from overflowing when BLE and the panel fight for
  the bus.
- Owns a static 1024-byte framebuffer in the device struct.
- Has a "fast path" that pushes the entire framebuffer in two I2C
  transactions (horizontal addressing) and a "slow path" for SH1106
  panels that need per-page writes with a +2 column offset.
- Probes an 800 kHz Fast-mode-plus (`Fm+`) device on the same bus at
  `init_panel` time. If the probe succeeds, the panel can be pushed
  at 800 kHz on demand when the 400 kHz push fails. If the probe
  fails, the slow (400 kHz) path stays in use. The 800 kHz fallback
  is logged with the proper "panel does not support Fm+" message on
  the second 800 kHz push failure.
- Provides boot bring-up helpers (multi-attempt pin/speed/address
  matrix, last-ditch GPIO-pair scan, bus recovery via SCL bit-bang)
  that the upstream doesn't expose in a usable form for this project.
- Ships a hand-ported GFX primitive subset that targets the
  1024-byte framebuffer directly (no per-pixel hal_ui_pixel
  round-trip on line draws).
- Exposes the existing `s_font5x7[95][5]` byte data as an Adafruit_GFX
  `GFXfont` so a future variable-width font can be dropped in without
  changing the render path.

## What it doesn't do

- No SPI transport. CEE-PEW's panel is I2C-only; SPI is a non-goal.
- No async / DMA / interrupt-driven writes. Synchronous only.
- No full Adafruit_GFX surface. We implement: `drawLine` (Bresenham),
  `drawRect` / `fillRect`, `drawCircle` / `fillCircle` (midpoint),
  `drawBitmap` (MSB-first, 1-bpp), and GFXfont-aware `write()`.
  Dropped: rounded-rect, triangle, getTextBounds, scroll ops.
- No C++ runtime. Pure C11. No vtable dispatch; flat C API.

## API surface

### Public header: `include/ceepew_oled.h`

Lifecycle, framebuffer access, display push, bring-up helpers:

- `ceepew_oled_create`, `ceepew_oled_destroy`
- `ceepew_oled_init_panel(bus, i2c_dev, addr)` — bus-handle plumbed
  through so the 800 kHz Fm+ fallback can add a second I2C device
  to the same bus; pass `NULL` for bus to disable the fallback.
- `ceepew_oled_multi_attempt(pins, &out_bus, &out_dev, &out_addr)` —
  bus-handle written back via the new `out_bus` parameter.
- `ceepew_oled_scan_all_pins(&out_bus, &out_dev, &out_addr)` — same.
- `ceepew_oled_get_buffer`, `ceepew_oled_get_buffer_size`,
  `ceepew_oled_clear_buffer`
- `ceepew_oled_display` (SSD1306 fast path, 2 I2C transactions)
- `ceepew_oled_display_sh1106(col_offset)` (SH1106 slow path,
  8 pages x 2 transactions)
- `ceepew_oled_push_tile(tile_col, tile_row)` — single 8x8-tile push,
  used by the tile-dirty fast path in `hal_ui_flush`. `tile_col` in
  [0, 15], `tile_row` in [0, 7]. **8 pages x 2 I2C transactions = 16
  transactions per tile.**
- `ceepew_oled_get_sh1106_mode`
- `ceepew_oled_bus_recover`, `ceepew_oled_probe_with_retry`,
  `ceepew_oled_bus_bringup`

### GFX primitives: `ceepew_oled_gfx_primitives.h`

- `ceepew_oled_gfx_line(dev, x0, y0, x1, y1, color)` — Bresenham,
  page-byte direct write (no per-pixel hal_ui_pixel round-trip).
  Endpoints are inclusive and need not be in any order; out-of-range
  endpoints are clamped, not rejected.
- `ceepew_oled_gfx_rect(dev, r, color)` — outline rectangle.
- `ceepew_oled_gfx_rect_fill(dev, r, color)` — filled rectangle.
- `ceepew_oled_gfx_circle(dev, cx, cy, radius, color)` — midpoint
  circle outline. Radius clamped to <= 63.
- `ceepew_oled_gfx_circle_fill(dev, cx, cy, radius, color)` —
  midpoint circle filled. Radius clamped to <= 63.
- `ceepew_oled_gfx_bitmap(dev, x, y, bitmap, w, h, color)` — 1-bpp
  packed bitmap blit, MSB-first (Adafruit_GFX convention). Pixels
  outside the panel are clipped, not rejected.
- `ceepew_oled_gfx_text(dev, x, y, str, color)` — text rendering via
  GFXfont-aware `write()`. Each glyph is 5x7 with 1 px column gap =
  6 px advance.
- `ceepew_oled_gfx_text_width(str)` — measure text width in pixels
  (6 px per printable ASCII character).

### Font adapter: `ceepew_oled_font_adapter.h`

Adafruit_GFX-format `GFXglyph` and `GFXfont` structs (renamed to
`ceepew_oled_GFXglyph_t` / `ceepew_oled_GFXfont_t` to avoid clashes
when the user has Adafruit headers in the include path):

- `ceepew_oled_GFXglyph_t` — single glyph descriptor.
- `ceepew_oled_GFXfont_t` — whole-font descriptor.
- `ceepew_oled_get_default_font()` — returns the bundled 5x7
  monospace font. Always non-NULL; the pointer is to `static const`
  in flash, so callers never own the storage.

The font's `bitmap` field points at a local 475-byte copy of the
5x7 monospace byte data compiled in by `ceepew_oled_font_adapter.c`.
The copy is byte-identical to the legacy `s_font5x7[95][5]` table
in `components/ceepew_hal/ui_manager.c`, so all rendered glyphs
match the legacy render path exactly. Keeping the data here means
the OLED rendering path has no link-time reference to `ceepew_hal`,
which keeps the component dependency graph a DAG. The cost is 475
bytes of flash duplication.

### Types: `hal_ui_types.h`

Shared types used by both this component and `ceepew_hal`:

- `HalUIColor_t` — pixel colour (`HAL_UI_COLOR_BLACK`,
  `HAL_UI_COLOR_WHITE`, `HAL_UI_COLOR_INVERT`).
- `HalUIRect_t` — `{ x, y, w, h }` rectangle.
- `CeePewErr_t` — project-wide error enum (moved here from
  `main/ceepew_assert.h` to break a `ceepew_oled` ↔ `main` circular
  `REQUIRES`).

### Assertions: `ceepew_assert.h`, `ceepew_assert.c`

Moved here from `main/` for the same circular-dependency reason. The
`CeePewErr_t` enum is now in `hal_ui_types.h` and is `#include`'d by
`ceepew_assert.h`. `ceepew_assert.c` does not include
`ceepew_config.h`; the `CEEPEW_DEBUG_SERIAL` gate uses `#ifdef`
because the symbol is `#define CEEPEW_DEBUG_SERIAL` (no value).

## 800 kHz Fast-mode-plus fallback

The 800 kHz fallback has two phases:

1. **Proactive probe at `init_panel` time.** `ceepew_oled_init_panel`
   calls `i2c_master_bus_add_device` to add a second
   `i2c_master_dev_handle_t` to the same bus at
   `CEEPEW_OLED_FAST_HZ` (800,000 Hz). A 100 ms probe timeout is
   used for the bus add. If the bus add or the panel ACK fails,
   the field `fast_probed` stays `false` and the 800 kHz path is
   disabled for the rest of the session.
2. **Reactive fallback at `ceepew_oled_display()` time.** The first
   push attempt always uses the slow (400 kHz) device. If that
   fails AND `fast_probed == true`, the component retries with the
   fast (800 kHz) device. On success, `fast_active` is set to
   `true` and all subsequent pushes use 800 kHz. On second failure,
   the component logs `"panel does not support Fm+; staying at
   400 kHz"` and stays on the slow path for the rest of the
   session.

This is observable at boot via `ESP_LOGI`/`ESP_LOGW`/`ESP_LOGE` lines
from the `OLED` tag. Lines you should see (in order, depending on
the panel's Fm+ support):

- `"OLED panel configured for 128x64"` — `init_panel` success.
- `"Panel ACKed at 800 kHz Fm+; fast mode available"` — proactive
  probe succeeded; 800 kHz is available.
- `"Panel did not ACK at 800 kHz Fm+; staying at 400 kHz"` —
  proactive probe failed; 800 kHz is disabled.
- `"Could not add 800 kHz device (rc=...); staying at 400 kHz"` —
  `i2c_master_bus_add_device` itself failed; 800 kHz is disabled.
- `"Retrying display push at 800 kHz Fm+"` — first 400 kHz push
  failed; attempting the 800 kHz fallback.
- `"Display push succeeded at 800 kHz Fm+; fast mode engaged"` —
  the 800 kHz push recovered the session.
- `"Display push at 800 kHz Fm+ failed: ...; panel does not support
  Fm+; staying at 400 kHz"` — both 400 kHz and 800 kHz failed; the
  component stays on the slow path for the rest of the session.

## Tile-dirty fast path

`components/ceepew_hal/hal_ui.c` keeps a 16-byte packed tile-dirty
bitmap (`s_tile_dirty[16]`, 16 column-tiles x 8 row-bits). Every
framebuffer write sets the corresponding tile bit. `hal_ui_flush`
counts dirty tiles and:

- If 1-8 tiles are dirty, calls `ssd1306_display_push_tiled`, which
  walks the bitmap and calls `ceepew_oled_push_tile` per dirty
  `(col, row)`.
- If >8 tiles are dirty (or the bitmap is all 0xFF), falls back to
  the full horizontal-addressing frame push.
- If a single tile-push fails, falls back to the full frame for
  that flush; the dirty bitmap is reset only on success.

The 8-tile threshold is conservative. The full-frame push uses
2 I2C transactions, while the tile path uses 16 transactions per
tile (8 pages x 2 transactions); measured cost on a typical
ESP32-WROOM-32 panel suggests the tile path is competitive up to
around 4-8 dirty tiles, depending on the bus speed (400 kHz vs
800 kHz) and the panel's command/data ACK latency. Use
`hal_ui_benchmark_flush()` (in `hal_ui.c`, gated by
`CEEPEW_DEBUG_SERIAL`) to measure the actual numbers on your
panel.

## Framebuffer geometry

- 8 pages x 128 columns = 1024 bytes.
- Vertical LSB-first: `page = y >> 3`, `bit = y & 0x07`,
  `byte_index = page * 128 + x`, `set pixel = byte_index |= 1 << bit`.
- Glyph data is column-major, bit 0 = top row, 5 bytes per glyph,
  identical to Adafruit's `glcdfont.c`. The 95-entry GFXglyph table
  points `bitmapOffset` at `i * 5` for glyph `i` in [0, 95).

## On-device tests

`components/ceepew_hal/ui_manager_test.c` includes 12 sub-tests
exercising the new GFX + font surface without needing the panel
hardware (each is logged as
`CEEPEW: ui_manager selftest - <NAME> PASS`):

- `gfx_line horiz` — Bresenham horizontal line at y=32 spanning
  the full width; only the affected page-bytes are non-zero, left
  and right neighbour pages remain zero.
- `gfx_line diag` — Bresenham diagonal from (0,0) to (63,63);
  verifies that the line's set pixels are spread across at least
  16 page-bytes (i.e. the line is not degenerating into a single
  vertical or horizontal line).
- `gfx_line vert` — Bresenham vertical line at x=64 spanning all
  64 rows; each of the 8 pages has exactly one bit set.
- `gfx_rect` — outline rectangle; verifies the interior page-bytes
  remain zero and the corners are correct.
- `gfx_rect_fill` — filled rectangle; verifies every column in the
  rectangle's page-byte range is set, and an overdraw (drawing
  BLACK over WHITE) clears the bits.
- `gfx_circle` — midpoint circle outline (radius 10, centred).
- `gfx_bitmap` — 8x1 MSB-first blit; expects byte 0x10 at
  `(0,0)`.
- `gfx_text` — renders "AB" and verifies >= 4 non-zero page-bytes
  in the bottom row.
- `gfx_text_width` — verifies `gfx_text_width(NULL) == 0`,
  `gfx_text_width("") == 0`, `gfx_text_width("A") == 6`,
  `gfx_text_width("Hello") == 30`.
- `font_adapter` — verifies `first == 0x20`, `last == 0x7E`,
  `yAdvance == 8`, `glyph[0].bitmapOffset == 0`,
  `glyph[94].bitmapOffset == 470`.
- `oled_buffer_lifecycle` — `create`, `clear`, `destroy` with
  no panel attached.
- `bus_recover` — calls `ceepew_oled_bus_recover` on the
  configured SDA/SCL pair and asserts the call returns within
  100 ms.

## No-malloc scope

`components/ceepew_oled/` is the **one** component in the CEE-PEW
firmware that does not enforce the project-wide no-malloc rule. This
is documented as an explicit exception in
`.github/copilot-instructions.md` (line 91) and
`.github/prompts/review-ceepew.prompt.md` (line 11) and
`.github/prompts/implement-module.prompt.md` (line 21). The exception
applies **only** to `components/ceepew_oled/`; every other component
must continue to use the 48 KB region pool (`components/mem/`).

The reason for the exception: this component is the lowest layer of
the OLED stack (no upstream dependencies within CEE-PEW), and the
GFX primitive / font / tile-dirty state are all `static const` data
in flash, so there is no allocation pressure from the new code. The
exception exists to give `ceepew_oled_init_panel`'s proactive 800 kHz
probe (which calls `i2c_master_bus_add_device`) the freedom to use
the new I2C master driver's internal allocation paths without
having to plumb the region pool into the IDF I2C driver. The 800 kHz
fallback runs at most once per session.

## Tunables

Defined at the top of `ceepew_oled.c` (file-static `#define`s):

- `CEEPEW_OLED_FAST_HZ` = 800000 (800 kHz Fm+ target)
- `CEEPEW_OLED_SLOW_HZ` = 400000 (400 kHz default)
- `CEEPEW_OLED_TILE_COLS` = 16
- `CEEPEW_OLED_TILE_ROWS` = 8
- `CEEPEW_OLED_I2C_TIMEOUT_TICKS` = 200 (200 ms at 1 kHz tick; in
  the public header `include/ceepew_oled.h`)
- `CEEPEW_OLED_PROBE_ATTEMPTS` = 3 (public header)
- `CEEPEW_OLED_PROBE_RETRY_MS` = 50 (public header)

## License

GPL-3.0-only. See [`/LICENSE`](../../LICENSE).

## Provenance

I2C transport patterns (multi-attempt bring-up, page-addressing
fallback, SCL bit-bang recovery, 800 kHz Fm+ probe) are derived from
[`nopnop2002/esp-idf-ssd1306`](https://github.com/nopnop2002/esp-idf-ssd1306)
(MIT) and adapted to the new `i2c_master_*` API. GFX primitive
algorithms (Bresenham, midpoint circle, MSB-first bitmap) are derived
from Adafruit_GFX (BSD-2-Clause).

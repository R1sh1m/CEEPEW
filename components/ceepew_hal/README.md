# components/ceepew_hal — HAL implementations + UI manager + display layer

This directory implements every public HAL API declared in `components/hal/`
and owns the UI runtime (OLED, input, RGB) and display-utility layer.

## Module groupings

| Group          | Files                                        | Role                                     |
|----------------|----------------------------------------------|------------------------------------------|
| GPIO/Init      | `hal_gpio.c`, `hal_init.c`                  | Pin init, ON semiconductor driver mux    |
| ADC/Input      | `hal_adc.c`, `hal_input.c`                  | Potentiometer ADC read, button debounce  |
| RNG            | `hal_rng.c`                                 | Hardware RNG init/fill                   |
| RGB            | `hal_rgb.c`                                 | RGB-LED pattern driver (no raw GPIO)     |
| Display        | `hal_ui.c`, `hal_oled.c`, `hal_oled_fonts.c`| SSD1306 OLED via `ssd1306` component     |
| Temp Sensor    | `hal_temp.c`                                | Internal temperature sensor              |
| UI Manager     | `ui_manager.c`, `ui_manager.h`              | 20+ state FSM (ui_manager.h is the API)  |

## Header path

`components/hal/` is added as `../hal` in this component's `CMakeLists.txt`
via `INCLUDE_DIRS`, making `#include "hal_pins.h"` work from any file in
`ceepew_hal` (and transitively from any component that `REQUIRES ceepew_hal`).

## Rules

- This directory contains `.c` implementations **only** — no public headers
  that other components consume directly.
- Public API contracts live in `components/hal/`. Do not declare a public
  symbol here that isn't first declared in `components/hal/`.
- Everything here can use `esp_log.h` but must **never** log key material.

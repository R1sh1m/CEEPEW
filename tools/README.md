# tools/

This folder contains PowerShell helper scripts and the IDF installer
config that the project does not need at build time. None of these
files are referenced by CMake; they are operator-facing.

## Contents

| File                          | Purpose                                                                                        |
|-------------------------------|------------------------------------------------------------------------------------------------|
| `verify_handoff_fix.ps1`      | Sprint 19 regression check. Build + optional flash to two devices + optional monitor.           |
| `eim_config.toml`             | ESP-IDF installer (`eim`) configuration used to install the toolchain at `C:\Espressif\tools`. |
| `README.md`                   | This file.                                                                                     |

## Usage

```powershell
. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1
.\tools\verify_handoff_fix.ps1 -Flash -Monitor
```

The companion `diagnose.ps1` script lives at the project root as a
thin shim — it is the primary entrypoint you reach for first when a
device is misbehaving. The scripts in this folder are diagnostic
adjuncts used during specific sprints or build-setup workflows.

## Open cleanup notes (for the next sprint)

- `components/transport/transport_stream.c` is a header-less, no-caller
  placeholder. It is included in the build via `components/transport/CMakeLists.txt`
  but no production code calls `transport_stream_send()`. Either add
  a `transport_stream.h` and a real caller, or remove it from the build.
- `components/transport/transport_ble_gatt_crypto.{c,h}` and
  `components/transport/transport_ble.{c,h}` are both in the build; the
  GATT-crypto wrapper is the newer code path. A future sprint should
  reconcile the two.

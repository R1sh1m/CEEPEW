# CEE-PEW VS Code Configuration Guide

## Overview

The `.vscode/` directory contains configuration for developing CEE-PEW (ESP32 E2E encrypted wireless communicator) in VS Code. All paths are hardcoded for your Windows setup with IDF v6.0.1.

## Configuration Files

### 1. **settings.json** - IDE Settings
- ESP-IDF Path: `C:\esp\v6.0.1\esp-idf`
- Python: `C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe`
- Serial Port: `COM5` (115200 baud)
- Compiler: Xtensa GCC (esp-15.2.0_20251204)
- Code Style: 4-space tabs, 80/120 column rulers, C11 + C++17

### 2. **c_cpp_properties.json** - IntelliSense
- Include Paths: Project headers + ESP-IDF components + Xtensa toolchain
- Compiler: Xtensa GCC
- IntelliSense Engine: Tag Parser (fastest)
- Defines: `ESP_PLATFORM`, `IDF_VER="6.0.1"`, `CEEPEW_PROJECT`

### 3. **launch.json** - Debugging
- Debugger: Xtensa GDB (gdbstub on serial)
- Pre-requisites:
  1. Enable gdbstub in menuconfig: `idf.py menuconfig`
     - Component config → ESP System Settings → Panic handler → **GDBStub on panic**
  2. Flash: `idf.py flash -p COM5`
  3. Run **idf: gdb** task to start GDB server
  4. Press F5 to attach VS Code debugger

### 4. **tasks.json** - Build Tasks
Accessible via `Ctrl+Shift+B` or Command Palette (`Ctrl+Shift+P` → "Tasks: Run Task"):

| Task | Description | Hotkey |
|------|-------------|--------|
| **idf: build** | Build firmware | Ctrl+Shift+B |
| **idf: flash** | Flash to ESP32 (COM5) | - |
| **idf: flash monitor** | Flash + open serial monitor | - |
| **idf: monitor** | Open serial monitor only | - |
| **idf: menuconfig** | Configure build options | - |
| **idf: fullclean** | Delete all build artifacts | - |
| **idf: gdb** | Start GDB server for debugging | - |

## Quick Start

### Build
```bash
Ctrl+Shift+B
# Or: Ctrl+Shift+P → Tasks: Run Task → idf: build
```

### Flash & Monitor
```bash
Ctrl+Shift+P → Tasks: Run Task → idf: flash monitor
```

### Debug Setup (GDBStub)
```bash
1. Enable gdbstub in menuconfig: idf.py menuconfig
2. Flash: Ctrl+Shift+P → idf: flash
3. Start GDB: Ctrl+Shift+P → idf: gdb
4. Attach debugger: F5 → Select "ESP32: Attach (idf.py gdb / gdbstub)"
5. Trigger crash or set breakpoints in GDB
```

## Toolchain Paths

All verified and installed:
- **ESP-IDF**: `C:\esp\v6.0.1\esp-idf`
- **GCC**: `C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin\xtensa-esp32-elf-gcc.exe`
- **GDB**: `C:\Espressif\tools\xtensa-esp-elf-gdb\16.3_20250913\xtensa-esp-elf-gdb\bin\xtensa-esp32-elf-gdb.exe`
- **Python**: `C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe`

## Troubleshooting

### Build fails with CMake error
- Check IDF v6.0.1 xtensa component bug (documented in build_blockers table)
- Workaround applied in `C:\esp\v6.0.1\esp-idf\tools\cmake\component.cmake`

### IntelliSense not working
- Rebuild IntelliSense: `Ctrl+Shift+P` → "C++: Rescan Solution"
- Verify `build/compile_commands.json` exists after build

### Debugger not connecting
- Ensure device is on COM5
- Run **idf: gdb** task first before pressing F5
- Check serial monitor for GDB output

### Serial monitor garbled output
- Verify baud rate: 115200 (configured in settings.json)
- Reinstall CP210x drivers if needed

## File Exclusions

The following are excluded from Explorer (`.vscode/settings.json`):
- `build/` - Build artifacts
- `.cache/` - CMake cache
- `**/*.pyc` - Python bytecode
- `**/__pycache__` - Python caches

## Extensions Required

Recommended VS Code extensions:
- **C/C++** (ms-vscode.cpptools)
- **CMake Tools** (ms-vscode.cmake-tools)
- **ESP-IDF** (espressif.esp-idf-extension)
- **Copilot** (github.copilot)
- **Git Graph** (mhutchie.git-graph)

## Notes

- All paths use absolute Windows-style paths (backslashes, drive letters)
- Serial port is hardcoded to COM5 for your dev setup
- CMake generator is Ninja (fast parallel builds)
- Compiler is Xtensa GCC (not clang, due to Windows ABI issues)

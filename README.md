# Keychron Battery Tray

[![Windows CI](https://github.com/mammadow/keychron-battery-tray/actions/workflows/ci.yml/badge.svg)](https://github.com/mammadow/keychron-battery-tray/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/mammadow/keychron-battery-tray)](https://github.com/mammadow/keychron-battery-tray/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

Native Windows system tray battery indicator and CLI diagnostic utility for Keychron wireless mice using the vendor HID protocol (confirmed with the Keychron Ultra-Link 8K receiver `VID 0x3434 / PID 0xD028` and Keychron M7 8K `0xD056`, USB interface `4`, Usage Page `0xFFC1`), with automatic fallbacks for standard HID Battery Strength (`0x06`/`0x20`) and Bluetooth LE Battery Service (`0x180F`/`0x2A19`).

Written in C17 directly against Win32, Windows HID, and CfgMgr32 APIs (`hid.dll`, `cfgmgr32.dll`, `shell32.dll`, `gdi32.dll`, `advapi32.dll`, loading `BluetoothApis.dll` on demand only when BLE fallback is active) for zero external runtime dependencies, compact binaries (`71.0 KB` tray / `42.0 KB` CLI), ~4.6 MB private memory footprint (~12.65 MB working set), and zero idle CPU wakeups.

## Tray Icon States

| Display | Meaning |
|---|---|
| `45` (White / Charcoal) | Normal live reading matching Windows dark / light taskbar theme |
| `25` (Yellow / Amber) | Recommended charge window reached (`<= 25%` Longevity / `<= 20%` Balanced) |
| `12` (Red) | Critical low battery (`<=` your low-battery alert threshold) |
| `45` (Green) | Charging below the recommended ceiling (or calibrating to `100%`), polls every `2m` |
| `80` (Cyan / Azure) | Recommended max charge ceiling reached (`>= 80%` / `>= 90%`) — unplug to protect battery |
| `45` (Muted gray) | Mouse asleep (`KC_HID_TIMEOUT`); keeps last known percentage until movement wakes it |
| `--` / `!` | Initial startup check (`--`) or device disconnected / I/O error (`!`) |

## CPU & RAM Usage

Measured live on Windows x64 (`keychron-battery-tray.exe`, Ultra-Link 8K `2.4GHz` session):

| Metric | Measured Footprint | Notes |
|---|---:|---|
| **Idle CPU** | **`0.00%`** (`0.00 ms` / 2s) | Blocked on `GetMessageW` + `WaitForMultipleObjects`; zero timer/busy loops |
| **Startup CPU** | **`~47 ms`** (`46.9 ms`) | Cold launch, initial HID poll, paired PID lookup, and first `HICON` render |
| **Private RAM** | **`~4.6 MB`** (`4.57 MB`) | Actual per-process private memory commit |
| **Working Set** | **`~12.65 MB`** (`12.65 MB`) | Includes shared Windows `SHELL32.dll` / `GDI32` / `CfgMgr32` pages |
| **Handles & GDI** | **`128` handles** | `17` GDI objects / `9` USER objects (`16`-slot MRU icon cache caps GDI growth) |
| **Loaded DLLs** | **`29` DLLs** | Uses `CfgMgr32`; avoids `SETUPAPI.dll`, `CRYPT32.dll`, and lazy-loads `BluetoothApis.dll` |
| **Binary Size** | **`71.0 KB` / `42.0 KB`** | `keychron-battery-tray.exe` (`71.0 KB`) + `keychron-battery.exe` (`42.0 KB`) |

## Features

- **Native Windows 11 typography**: renders `Segoe UI Variable Text` Regular at `0.75×` tray icon size (`12 DIPs`, falling back to `Segoe UI` on Windows 10) into a `16`-slot MRU in-memory `CreateIconIndirect` `HICON` cache (zero temp `.ico` files, bounded GDI handles).
- **Charging & Battery Protector (Charge Cycle Manager)**: decodes byte `20` charging status (`0x80`), polls every `2m` while charging, and helps extend Li-ion lifespan with:
  - **Recommended charge bands**: `Longevity (25%–80%)` (default) or `Balanced (20%–90%)` or `Off`. Turns the tray number **yellow/amber** at the recommended plug-in floor (`25%`) and **cyan/blue** with an unplug notification at the charge ceiling (`80%`). *(Note: neither Keychron mouse firmware nor standard Windows USB host ports provide a software command to cut 5V VBUS power per port without disconnecting USB data, so capping uses visual/toast unplug protection.)*
  - **Laptop battery guard**: uses Windows `GetSystemPowerStatus` and `PBT_APMPOWERSTATUSCHANGE` to detect laptop battery vs AC power, warning earlier when charging the mouse drains a low laptop battery (`<= 25%` while mouse `>= 50%`) and reminding you at `80%`.
  - **Calibration cycle manager**: tracks cumulative `%` charged (with trickle-wobble deadband) and calendar days in Registry; recommends a `100%` fuel-gauge calibration every `10` equivalent cycles (for high-drain 8K use) or `90` days quarterly (`>= 1` cycle used; `180`-day max idle fallback), lifting the `80%` alert until `100%` is reached and resetting counters automatically.
- **Active drain estimator**: tracks discharging usage across connected intervals (excluding idle sleep and offline gaps) to estimate `%/h` drain rate and remaining active hours in the tray menu.
- **Event-driven wakeups & instant exit**: uses `WM_POWERBROADCAST` resume, `GUID_CONSOLE_DISPLAY_STATE` Modern Standby wake (60s cooldown), VID-filtered `WM_DEVICECHANGE` hotplug (`1.5s` settle on USB unplug), on-demand Raw Input mouse wake (`VID_3434` only — ignores touchpads and other mice, and scales prolonged sleep backoff up to your `Poll interval` while armed), `TaskbarCreated` recovery, and `CancelIoEx` cancellation on quit.
- **Per-Monitor V2 DPI & theme aware**: adapts live to `WM_DPICHANGED`, `WM_DISPLAYCHANGE`, and `ImmersiveColorSet` (`SystemUsesLightTheme`).
- **Lightweight `CfgMgr32` enumeration & caching**: avoids loading `SETUPAPI.dll` or `BluetoothApis.dll` in USB/2.4GHz sessions and caches both vendor HID interfaces and BLE GATT `0x180F` paths across polls and `--watch` runs.
- **Persisted tray preferences**: stores settings (`PollMinutes`, `LowAlertPct`, `NotifyFull`, `TargetPid`, `TargetInterface`) and Battery Protector state (`ProtectorMode`, `ProtectorLaptopGuard`, `ProtectorCalibrating`, `ProtectorCumChargedPct`, `ProtectorLastCalUnix`, `ProtectorSnoozeUntil`) under `HKCU\Software\KeychronBatteryTray` and optional auto-start under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.

## Layout

| Path | Purpose |
|---|---|
| `src/` | Native C17 implementation (`protocol`, `glyph_blit`, `hid_win`, `icon_render`, `settings_win`, `tray_main`, `cli_main`) |
| `assets/` | Windows PerMonitorV2 DPI manifest, application `.ico`, and `.rc` version resources |
| `tests/` | CTest suites (`test_core.c`, `test_win.c`) |
| `packaging/scoop/` | Scoop package manifest template |
| `.github/workflows/` | MinGW (Release/Debug) + MSVC CI and automated tag-release workflows |
| `legacy/` | Original Python scripts (`keychron_battery.py`, `keychron_battery_tray_native.pyw`, `requirements.txt`) |

## Download & Install

Grab the latest `keychron-battery-win64-v*.zip` from [GitHub Releases](https://github.com/mammadow/keychron-battery-tray/releases) (built by CI with `SHA256SUMS.txt`), extract it, and run `keychron-battery-tray.exe`.

Or install directly via [Scoop](https://scoop.sh) using the auto-generated release manifest (populated with each release's exact SHA-256):

```powershell
scoop install https://github.com/mammadow/keychron-battery-tray/releases/latest/download/keychron-battery-tray.json
```

## Build & Test (MinGW-W64 or MSVC)

### MinGW-W64 + Ninja (CMake Presets)

```powershell
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release --verbose
```

### Visual Studio / MSVC (x64 Native Tools Command Prompt)

```powershell
cmake -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-msvc --parallel
ctest --test-dir build-msvc --output-on-failure --verbose
```

Artifacts produced:
- `keychron-battery-tray.exe` — GUI tray app (no console window)
- `keychron-battery.exe` — CLI diagnostic & scripting utility

## Usage

### Tray Icon (Windows)

Run `.\build\keychron-battery-tray.exe` (CLI target overrides `--vid`, `--pid`, `--interface` take runtime precedence and are preserved when enabling startup).

- **Left-click**: refresh battery reading immediately.
- **Right-click**:
  - View device name, percentage, active link (`2.4GHz` / `USB` / `BLE`), reading timestamp (or asleep state), and active drain estimate when available
  - **Refresh now**
  - **Battery Protector** (cycle/day counter, laptop AC/battery status, `Longevity 25%–80%` / `Balanced 20%–90%` / `Off`, `Laptop battery guard`, `Calibrate battery to 100%`, `Snooze 14 days`, `Reset cycle counter`)
  - **Device** (`Auto-detect` or pinned Keychron PID / interface)
  - **Poll interval** (`5m`, `15m`, `30m`, `60m`)
  - **Alerts** (`Low: Off` / `Below 10%` / `Below 15%` / `Below 20%`, plus `Notify when full (100%)`)
  - **Run at Windows startup**
  - **Copy diagnostics** (copies structured JSON status to clipboard)
  - **Quit**

### CLI Tool

```powershell
.\build\keychron-battery.exe             # auto-detect -> "Battery: 43% (2.4GHz)"
.\build\keychron-battery.exe --watch 2   # poll every 2s until Ctrl+C
.\build\keychron-battery.exe --raw       # bare integer percentage (e.g. 43)
.\build\keychron-battery.exe --json      # structured JSON for scripts
.\build\keychron-battery.exe --list      # list detected Keychron HID interfaces
.\build\keychron-battery.exe --list --json
.\build\keychron-battery.exe --pid 0xd028 --interface 4 --debug
```

Sample `--json` output:

```json
{"ok":true,"vid":"0x3434","pid":"0xd028","interface":4,"product":"Keychron M7 8K","paired_pid":"0xd056","link":"2.4GHz","battery":43,"charging":false}
```

| Flag | Description |
|---|---|
| `--list` | List Keychron HID interfaces and exit |
| `--watch [seconds]` | Poll continuously (`1–3600s`, default `5s`) until `Ctrl+C` |
| `--vid <hex\|dec>` | Vendor ID (default: `0x3434`) |
| `--pid <hex\|dec>` | Product ID (auto-detects if omitted) |
| `--interface <n>` | USB interface number `0–255` (auto-detects if omitted) |
| `--timeout <ms>` | Per-attempt HID response timeout (`100–10000`, default `1500`) |
| `--raw` | Print bare percentage integer only |
| `--json` | Structured JSON output (compatible with query, `--watch`, and `--list`) |
| `--debug` | Print raw HID caps, transport method, and packet bytes to stderr |
| `--version`, `-v` | Print version and exit |
| `--help`, `-h` | Show usage help |

## Protocol Notes

Based on Keychron's vendor HID protocol documented in [csutcliff/keychron-battery-dkms](https://github.com/csutcliff/keychron-battery-dkms) and live hardware captures:

- **Battery status (`0xB3 0x06` → `0xB4 0x06`)**: open the vendor HID interface (`MI_04`, `UsagePage == 0xFFC1` for Ultra-Link 8K / wired mice), send a zero-padded `64`-byte report (`WriteFile` / `HidD_SetFeature`) starting with `[0xB3, 0x06]`, and decode byte `20` from the `0xB4 0x06` input report (bit `7` / `0x80` = charging, bits `0–6` / `0x7F` = `0–100%`).
- **Paired mouse lookup (`0xB5 0x03` → `0xB6/0xB5 0x03`)**: send `[0xB5, 0x03]` to the Ultra-Link 8K receiver (`0xD028`) to read up to `3` paired slots starting at byte offset `3` (stride `5`: little-endian `VID`, `PID`, and link state). Confirmed built-in mouse PIDs: `0xD048` (`Keychron M5`), `0xD049` (`Keychron M6 8K`), `0xD056` (`Keychron M7 8K`).

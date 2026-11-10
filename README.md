# Keychron Battery Tray

[![Windows CI](https://github.com/mammadow/keychron-battery-tray/actions/workflows/ci.yml/badge.svg)](https://github.com/mammadow/keychron-battery-tray/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/mammadow/keychron-battery-tray)](https://github.com/mammadow/keychron-battery-tray/releases)

Native Windows system tray battery indicator and CLI diagnostic utility for Keychron wireless mice using the vendor HID protocol (confirmed with the Keychron Ultra-Link 8K receiver, VID `0x3434`, PID `0xD028`, USB interface `4`, Usage Page `0xFFC1`).

Written in C17 against the Win32 and Windows HID SDKs (`hid.dll`, `setupapi.dll`, `shell32.dll`, `gdi32.dll`, `advapi32.dll`) for zero external runtime dependencies, ~1.7 MB private memory footprint, and zero idle polling wakeups.

## Features

- **Native Windows 11 typography**: renders `Segoe UI Variable Text` Regular at `0.75×` tray icon size (`12 DIPs`, falling back to `Segoe UI` on Windows 10) with hinted grayscale anti-aliasing and automatic width-fitting for `"100"` directly into an in-memory `CreateIconIndirect` `HICON` cache (no temp `.ico` files on disk).
- **Per-Monitor V2 DPI & light/dark taskbar aware**: responds live to `WM_DPICHANGED`, `WM_DISPLAYCHANGE`, and `ImmersiveColorSet` (`SystemUsesLightTheme`).
- **Event-driven & zero busy-wait**: blocks on Win32 messages and kernel event waits; supports left-click refresh, `WM_POWERBROADCAST` resume, `GUID_CONSOLE_DISPLAY_STATE` Modern Standby display wake (60s cooldown), `WM_DEVICECHANGE` HID plug/unplug hotplug, and `TaskbarCreated` Explorer recovery.
- **Instant quit & sleep resilience**: cancels pending `OVERLAPPED` HID I/O immediately on exit via `CancelIoEx`, holds last reading across brief wireless sleep misses, and backs off (`30s -> 60s -> 120s -> 300s`).
- **Configurable tray menu**: shows detected device name + timestamp, **Poll interval** (`5m` / `15m` / `30m` / `60m`), **Low-battery alert** (`Off` / `10%` / `15%` / `20%`), and **Run at Windows startup** (persisted under `HKCU\Software\KeychronBatteryTray` / `HKCU\...\Run`).
- **Scriptable CLI (`keychron-battery.exe`)**: auto-detects the Keychron vendor interface by default and supports `--list`, `--raw`, `--json`, `--timeout`, `--vid`, `--pid`, `--interface`, and `--debug`.

## Layout

| Path | Purpose |
|---|---|
| `src/` | Native C17 implementation (`protocol`, `glyph_blit`, `hid_win`, `icon_render`, `settings_win`, `tray_main`, `cli_main`) |
| `assets/` | Windows PerMonitorV2 DPI manifest and `.rc` resource |
| `tests/` | CTest suites (`test_core.c`, `test_win.c`) |
| `.github/workflows/` | Windows MinGW-W64 CMake/CTest CI workflow |
| `legacy/` | Original Python scripts (`keychron_battery.py`, `keychron_battery_tray_native.pyw`, `requirements.txt`) |

## Download (Prebuilt)

Grab the latest `keychron-battery-win64-v*.zip` from [GitHub Releases](https://github.com/mammadow/keychron-battery-tray/releases) (built automatically by CI with SHA-256 checksums), extract it, and run `keychron-battery-tray.exe`.

## Build & Test (CMake + MinGW-W64 / Ninja)

```powershell
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release --verbose
```

Or without presets:

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Artifacts produced in `build/`:
- `build/keychron-battery-tray.exe` — GUI tray app (no console window)
- `build/keychron-battery.exe` — CLI diagnostic & scripting utility

## Usage

### Tray Icon (Windows)

Run `.\build\keychron-battery-tray.exe` (or right-click the tray icon and enable **Run at Windows startup**). Optional target overrides (`--vid`, `--pid`, `--interface`) are preserved automatically when toggling Windows startup.

- **Left-click**: refresh battery reading immediately.
- **Right-click**: view device status, refresh now, pick poll interval (`5m`–`60m`), pick low-battery alert threshold, toggle startup, or quit.

### CLI Tool

```powershell
.\build\keychron-battery.exe             # auto-detect and print "Battery: 46%"
.\build\keychron-battery.exe --raw       # print bare percentage integer (e.g. 46)
.\build\keychron-battery.exe --json      # structured JSON for scripts
.\build\keychron-battery.exe --list      # list detected Keychron HID interfaces
.\build\keychron-battery.exe --list --json
.\build\keychron-battery.exe --pid 0xd028 --interface 4 --debug
```

## Protocol Notes

Based on the Keychron vendor HID protocol reverse-engineered in [csutcliff/keychron-battery-dkms](https://github.com/csutcliff/keychron-battery-dkms):

- Open the vendor-specific HID interface (`interface_number == 4`, `UsagePage == 0xFFC1` for the Ultra-Link 8K receiver).
- Send a `64`-byte report (`WriteFile` / `HidD_SetFeature`) with report ID `0xB3` and command `0x06`.
- Read a `64`-byte response starting with `0xB4`, `0x06`, with the battery percentage (`0–100`) at byte offset `20`.

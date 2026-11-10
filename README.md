# Keychron Battery Tray

Windows system tray battery indicator and CLI diagnostic utility for Keychron wireless mice using the vendor HID protocol (confirmed with the Keychron Ultra-Link 8K receiver, VID `0x3434`, PID `0xD028`, USB interface `4`).

## Features

- **Native-style tray icon** (`keychron_battery_tray_native.pyw`): renders the battery percentage as crisp white Segoe UI text (4x supersampled and downsampled with `BOX` filtering) sized to Windows `SM_CXSMICON` with a transparent background.
- **Immediate startup reading**: performs the first battery query inside `pystray`'s `setup` callback so the real percentage appears right away instead of `--`.
- **Auto-polling & manual refresh**: polls every 30 minutes (retries every 30 seconds if unavailable), left-click the tray icon or right-click **Refresh now** to query immediately, and hover for exact status. Shows `!` on error.
- **CLI diagnostic script** (`keychron_battery.py`): enumerate Keychron HID interfaces and query battery status from the terminal with optional raw send/receive debug output.

## Files

| File | Purpose |
|---|---|
| `keychron_battery_tray_native.pyw` | Windows taskbar tray icon app (runs silently with `pythonw`) |
| `keychron_battery.py` | CLI tool to list Keychron HID interfaces and test battery queries |

## Installation

```powershell
pip install -r requirements.txt
```

Dependencies: `hidapi`, `pystray`, `pillow`.

## Usage

### Tray Icon (Windows)

Run without a console window:

```powershell
pythonw keychron_battery_tray_native.pyw
```

To start automatically on login, press `Win + R`, run `shell:startup`, and place a shortcut to `keychron_battery_tray_native.pyw` in that folder.

### CLI Tool

List all detected Keychron HID interfaces:

```powershell
python keychron_battery.py --list
```

Query a specific device (example for PID `0xD028`, interface `4`):

```powershell
python keychron_battery.py --pid 0xd028 --interface 4
```

Pass `--debug` to print raw HID feature/output write attempts and incoming report bytes.

## Protocol Notes

Based on the Keychron vendor HID protocol reverse-engineered in [csutcliff/keychron-battery-dkms](https://github.com/csutcliff/keychron-battery-dkms):

- Open the vendor-specific HID interface (`interface_number == 4` for the Ultra-Link 8K receiver).
- Send a 64-byte report with report ID `0xB3` and command `0x06`.
- Read a 64-byte response starting with `0xB4`, `0x06`, with the battery percentage (`0–100`) at byte offset `20`.

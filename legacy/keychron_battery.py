#!/usr/bin/env python3
"""
Read the battery level from a Keychron wireless mouse (M5, M7 8K, etc.)
using the vendor-specific HID protocol that Keychron's Launcher/software uses.

Protocol (reverse-engineered by github.com/csutcliff/keychron-battery-dkms):
  - Talk to USB interface 4 (the vendor-specific HID interface, not the
    normal mouse-input or keyboard-input interfaces).
  - Send a 64-byte FEATURE report: byte0 = 0xB3 (report id), byte1 = 0x06 (cmd).
  - Read back a 64-byte INPUT report: byte0 = 0xB4, byte1 = 0x06 (echo),
    byte20 = battery percentage (0-100).

Requires: pip install hidapi --break-system-packages   (or just `pip install hidapi`)

Usage:
  python3 keychron_battery.py            # auto-detect any Keychron vendor interface
  python3 keychron_battery.py --list     # list all HID interfaces exposed by Keychron devices
  python3 keychron_battery.py --vid 0x3434 --pid 0xXXXX --interface 4
"""

import argparse
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("Missing dependency. Install with: pip install hidapi --break-system-packages")

KEYCHRON_VID = 0x3434
REPORT_ID_CMD = 0xB3
REPORT_ID_RESP = 0xB4
CMD_STATUS = 0x06
BATTERY_OFFSET = 20
REPORT_SIZE = 64
VENDOR_INTERFACE_GUESS = 4  # matches M5 / Ultra-Link receiver; may differ for M7 8K


def list_keychron_interfaces():
    devices = [d for d in hid.enumerate() if d["vendor_id"] == KEYCHRON_VID]
    if not devices:
        print("No Keychron HID devices found. Is the mouse/receiver plugged in "
              "or connected via 2.4G/Bluetooth?")
        return devices
    print(f"{'PID':<8}{'Interface':<11}{'Usage Page':<12}{'Usage':<8}Product")
    for d in devices:
        print(f"{hex(d['product_id']):<8}{d['interface_number']:<11}"
              f"{hex(d.get('usage_page', 0)):<12}{hex(d.get('usage', 0)):<8}"
              f"{d.get('product_string','?')}")
    return devices


def query_battery(vid, pid, interface_number, path=None, retries=3, timeout=1.5, debug=False):
    devices = hid.enumerate(vid, pid)
    target = None
    if path:
        target = next((d for d in devices if d["path"] == path), None)
    else:
        # Prefer the given interface number; fall back to any non-mouse-input interface.
        target = next((d for d in devices if d["interface_number"] == interface_number), None)

    if not target:
        raise RuntimeError(
            f"Could not find interface {interface_number} on VID={hex(vid)} PID={hex(pid)}. "
            "Run with --list to see available interfaces and pick the right one."
        )

    dev = hid.device()
    dev.open_path(target["path"])
    dev.set_nonblocking(False)

    try:
        for attempt in range(retries):
            if debug:
                print(f"[debug] attempt {attempt + 1}/{retries}")

            # Try both send methods; report which ones actually succeed.
            report = [0x00] * REPORT_SIZE
            report[0] = REPORT_ID_CMD
            report[1] = CMD_STATUS

            sent_ok = False
            try:
                n = dev.send_feature_report(bytes(report))
                if debug:
                    print(f"[debug] send_feature_report -> {n} bytes")
                sent_ok = n and n > 0
            except OSError as e:
                if debug:
                    print(f"[debug] send_feature_report failed: {e} / hid_error: {dev.error()}")

            try:
                n = dev.write(bytes(report))
                if debug:
                    print(f"[debug] write -> {n} bytes")
                sent_ok = sent_ok or (n and n > 0)
            except OSError as e:
                if debug:
                    print(f"[debug] write failed: {e} / hid_error: {dev.error()}")

            if not sent_ok and debug:
                print("[debug] both send methods failed — device may need exclusive access "
                      "(close Keychron Launcher / any tab using it) or a different interface.")

            deadline = time.time() + timeout
            while time.time() < deadline:
                data = dev.read(REPORT_SIZE, timeout_ms=200)
                if not data:
                    continue
                if debug:
                    print(f"[debug] read {len(data)} bytes: {list(data)}")
                if data[0] == REPORT_ID_RESP and len(data) > BATTERY_OFFSET \
                        and data[1] == CMD_STATUS and data[BATTERY_OFFSET] <= 100:
                    return data[BATTERY_OFFSET]
            time.sleep(0.1)
        raise TimeoutError("No valid battery response — mouse may be asleep/off, or this PID "
                            "uses a different report format. Try --debug for more detail.")
    finally:
        dev.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="List Keychron HID interfaces and exit")
    ap.add_argument("--vid", type=lambda x: int(x, 0), default=KEYCHRON_VID)
    ap.add_argument("--pid", type=lambda x: int(x, 0), default=None,
                     help="Product ID, e.g. 0xd028. Use --list to find it.")
    ap.add_argument("--interface", type=int, default=VENDOR_INTERFACE_GUESS,
                     help="USB interface number of the vendor-specific HID endpoint")
    ap.add_argument("--debug", action="store_true", help="Print raw send/receive details")
    args = ap.parse_args()

    if args.list or args.pid is None:
        list_keychron_interfaces()
        if args.pid is None:
            print("\nPass --pid <value> (and --interface <n> if needed) once you've "
                  "identified the right entry above.")
        return

    battery = query_battery(args.vid, args.pid, args.interface, debug=args.debug)
    print(f"Battery: {battery}%")


if __name__ == "__main__":
    main()
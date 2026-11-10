"""
Keychron mouse battery - Windows tray icon

- Segoe UI text rendered large then downsampled with BOX filtering for
  bright, solid strokes at tiny tray-icon sizes
- Left-click the icon to force a refresh
- Auto-refreshes every 30 minutes (30 seconds on error)
- Shows the real battery number immediately on startup (see note below)

Install deps:
    pip install hidapi pystray pillow

Run:
    pythonw keychron_battery_tray.pyw
    (rename to .pyw, or run `pythonw keychron_battery_tray.py`, to avoid a console window)
"""

import sys
import threading
import time
import os
import ctypes

try:
    import hid
except ImportError:
    sys.exit("Missing hidapi. Install with: pip install hidapi")

try:
    import pystray
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Missing pystray/pillow. Install with: pip install pystray pillow")

# ------------------------------------------------------------------
# Battery protocol (confirmed working: Keychron Ultra-Link 8K receiver)
# ------------------------------------------------------------------
VENDOR_ID = 0x3434
PRODUCT_ID = 0xD028
INTERFACE_NUMBER = 4
REPORT_ID_CMD, REPORT_ID_RESP, CMD_STATUS = 0xB3, 0xB4, 0x06
BATTERY_OFFSET = 20
REPORT_SIZE = 64
POLL_INTERVAL_SEC = 30 * 60      # 30 minutes
RETRY_INTERVAL_SEC = 30          # quick retry if a query failed


def find_device_path():
    for d in hid.enumerate(VENDOR_ID, PRODUCT_ID):
        if d.get("interface_number") == INTERFACE_NUMBER:
            return d["path"]
    return None


def query_battery_once():
    path = find_device_path()
    if not path:
        raise RuntimeError("Device not found")
    dev = hid.device()
    dev.open_path(path)
    dev.set_nonblocking(False)
    try:
        for _ in range(3):
            report = [0] * REPORT_SIZE
            report[0] = REPORT_ID_CMD
            report[1] = CMD_STATUS
            dev.write(bytes(report))
            deadline = time.time() + 1.5
            while time.time() < deadline:
                data = dev.read(REPORT_SIZE, timeout_ms=200)
                if not data:
                    continue
                if data[0] == REPORT_ID_RESP and data[1] == CMD_STATUS \
                        and data[BATTERY_OFFSET] <= 100:
                    return data[BATTERY_OFFSET]
            time.sleep(0.2)
        raise TimeoutError("No response")
    finally:
        dev.close()


# ------------------------------------------------------------------
# Icon rendering
# ------------------------------------------------------------------

SM_CXSMICON = 49


def _get_icon_size():
    try:
        size = ctypes.windll.user32.GetSystemMetrics(SM_CXSMICON)
        if size and size > 0:
            return size
    except Exception:
        pass
    return 16  # sane fallback (100% scaling)


ICON_SIZE = _get_icon_size()


def _find_bold_font():
    windir = os.environ.get("WINDIR", r"C:\Windows")

    candidates = [
        os.path.join(windir, "Fonts", "seguisb.ttf"),
        os.path.join(windir, "Fonts", "segoeui.ttf"),
    ]

    for fp in candidates:
        if os.path.isfile(fp):
            return fp

    return None


_FONT_PATH = _find_bold_font()


def make_icon_image(text):
    """
    Windows 11-style percentage icon.

    Render large with Segoe UI, then downsample with BOX.
    BOX is intentionally used instead of LANCZOS because it preserves
    bright, solid strokes at tiny tray-icon sizes.
    """

    size = ICON_SIZE

    # Render at 4x resolution.
    scale = 4
    big_size = size * scale

    # Windows 11 uses a fairly normal-looking Segoe UI weight.
    font_size = round(size * 0.72 * scale)

    font = ImageFont.truetype(
        _FONT_PATH,
        font_size
    )

    img = Image.new(
        "RGBA",
        (big_size, big_size),
        (0, 0, 0, 0)
    )

    draw = ImageDraw.Draw(img)

    # Get text dimensions.
    bbox = draw.textbbox(
        (0, 0),
        text,
        font=font
    )

    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]

    # Center horizontally.
    x = (big_size - w) // 2 - bbox[0]

    # Windows-style slightly-high vertical placement.
    y = (big_size - h) // 2 - bbox[1] - round(size * 0.04 * scale)

    draw.text(
        (x, y),
        text,
        font=font,
        fill=(255, 255, 255, 255)
    )

    # BOX averaging keeps the strokes much brighter than LANCZOS.
    img = img.resize(
        (size, size),
        Image.Resampling.BOX
    )

    return img


def make_status_icon(symbol):
    """Small glyph (e.g. '!') for error states — same direct-render path."""
    return make_icon_image(symbol)


# ------------------------------------------------------------------
# Tray icon with left-click-to-refresh
# ------------------------------------------------------------------
class RefreshableIcon(pystray.Icon):
    def __init__(self, *args, refresh_callback=None, **kwargs):
        super().__init__(*args, **kwargs)
        self._refresh_callback = refresh_callback

    def _on_notify(self, wparam, lparam):
        if lparam == 0x201 and self._refresh_callback:  # WM_LBUTTONDOWN
            self._refresh_callback()
        super()._on_notify(wparam, lparam)


class BatteryTray:
    def __init__(self):
        self._stop = threading.Event()
        self._force = threading.Event()

        def refresh(*_):
            self._force.set()

        self.icon = RefreshableIcon(
            "keychron_battery",
            make_icon_image("--"),
            "Keychron mouse battery: unknown",
            menu=pystray.Menu(
                pystray.MenuItem("Refresh now", refresh),
                pystray.MenuItem("Quit", self.quit),
            ),
            refresh_callback=refresh,
        )

    def quit(self, *_):
        self._stop.set()
        self._force.set()
        self.icon.stop()

    def _update(self):
        try:
            pct = query_battery_once()
            self.icon.icon = make_icon_image(str(pct))
            self.icon.title = f"Keychron mouse battery: {pct}%"
            return True
        except Exception as e:
            self.icon.icon = make_status_icon("!")
            self.icon.title = f"Keychron mouse battery: unavailable ({e})"
            return False

    def _worker(self):
        while not self._stop.is_set():
            ok = self._update()
            wait = POLL_INTERVAL_SEC if ok else RETRY_INTERVAL_SEC
            for _ in range(wait * 10):
                if self._stop.is_set() or self._force.is_set():
                    break
                time.sleep(0.1)
            self._force.clear()

    def _setup(self, icon):
        # pystray only guarantees icon updates are actually delivered to
        # the shell once we're inside this setup callback (i.e. after the
        # tray icon is fully registered). Setting icon.icon/.title earlier
        # — e.g. from a thread started before icon.run() — races Windows'
        # own setup and the update can get silently dropped, which is why
        # it showed "--" until the next manual refresh landed one cycle
        # later. Doing the first update here, then handing off to the
        # normal polling loop, fixes that.
        icon.visible = True
        self._worker()

    def run(self):
        self.icon.run(setup=self._setup)


if __name__ == "__main__":
    BatteryTray().run()
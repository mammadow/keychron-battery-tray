#include "hid_win.h"
#include "icon_render.h"
#include "protocol.h"
#include "settings_win.h"
#include "updater_win.h"
#include "version.h"

#include <dbt.h>
#include <hidsdi.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WM_TRAY_CALLBACK           (WM_USER + 11)
#define WM_APP_POLL_RESULT         (WM_APP + 1)
#define WM_APP_UPDATE_CHECK_DONE   (WM_APP + 2)
#define WM_APP_UPDATE_INSTALL_DONE (WM_APP + 3)

#define IDI_APP_ICON          101U
#define TRAY_ICON_UID         1U
#define TIMER_RESUME_REFRESH  1U
#define TIMER_USB_REFRESH     2U
#define TIMER_INPUT_WAKE      3U

#define KC_RAW_CACHE_CAP      8U

#define IDM_STATUS            1001U
#define IDM_REFRESH           1002U
#define IDM_AUTOSTART         1003U
#define IDM_QUIT              1004U
#define IDM_VERSION           1005U
#define IDM_COPY_DIAG         1006U
#define IDM_STATUS_EST        1007U
#define IDM_CHECK_UPDATES     1008U

#define KC_WIDE2(x)           L##x
#define KC_WIDE(x)            KC_WIDE2(x)

#define IDM_POLL_5            1101U
#define IDM_POLL_15           1102U
#define IDM_POLL_30           1103U
#define IDM_POLL_60           1104U

#define IDM_ALERT_OFF         1201U
#define IDM_ALERT_10          1202U
#define IDM_ALERT_15          1203U
#define IDM_ALERT_20          1204U
#define IDM_ALERT_FULL        1205U

#define IDM_DEV_AUTO          1300U
#define KC_MENU_DEV_MAX       16U

#define IDM_PROT_INFO1        1401U
#define IDM_PROT_INFO2        1402U
#define IDM_PROT_INFO3        1403U
#define IDM_PROT_LONGEVITY    1410U
#define IDM_PROT_BALANCED     1411U
#define IDM_PROT_OFF          1412U
#define IDM_PROT_LAPTOP_GUARD 1413U
#define IDM_PROT_CAL_TOGGLE   1420U
#define IDM_PROT_CAL_SNOOZE   1421U
#define IDM_PROT_CAL_RESET    1422U

#define KC_DISPLAY_WAKE_COOLDOWN_MS 60000ULL

/* GUID_CONSOLE_DISPLAY_STATE from winnt.h without requiring <initguid.h>. */
static const GUID KC_GUID_CONSOLE_DISPLAY_STATE = {
    0x6fe69556U, 0x704aU, 0x47a0U, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}
};

typedef struct kc_raw_dev_entry {
    HANDLE hdevice;
    bool is_match;
} kc_raw_dev_entry_t;

typedef enum kc_upd_ui_state {
    KC_UPD_IDLE = 0,
    KC_UPD_CHECKING = 1,
    KC_UPD_INSTALLING = 2
} kc_upd_ui_state_t;

typedef struct kc_tray_app {
    HINSTANCE hinst;
    HWND hwnd;
    HANDLE mutex;
    HDEVNOTIFY dev_notify;
    HPOWERNOTIFY power_notify;
    UINT wm_taskbar_created;

    HANDLE worker_thread;
    HANDLE wake_event;
    HANDLE stop_event;
    CRITICAL_SECTION state_lock;

    uint16_t target_vid;
    uint16_t target_pid;
    int target_interface;

    kc_tray_settings_t settings;
    kc_protector_persist_t prot_persist;
    kc_protector_runtime_t prot_rt;
    bool has_sys_battery;
    bool on_laptop_battery;
    uint8_t laptop_pct;

    kc_hid_session_t hid_session;
    uint32_t session_epoch;
    kc_poll_state_t poll_state;
    kc_icon_cache_t icon_cache;

    bool tray_added;
    bool refresh_in_flight;
    bool pending_refresh;
    bool cli_target_override;
    kc_hid_status_t last_status;
    SYSTEMTIME last_reading_time;
    bool has_reading_time;
    wchar_t device_product[128];

    char last_glyph[8];
    wchar_t last_tip[128];
    HICON last_hicon;
    int icon_size;
    bool light_taskbar;
    kc_icon_tone_t last_tone;

    bool raw_watch_armed;
    kc_raw_dev_entry_t raw_cache[KC_RAW_CACHE_CAP];
    size_t raw_cache_count;

    bool menu_open;
    kc_upd_ui_state_t update_ui_state;
    HANDLE update_thread;
} kc_tray_app_t;

static kc_tray_app_t g_app;

static void kc_set_raw_mouse_watch(kc_tray_app_t *app, bool enable);

static void kc_parse_tray_args(kc_tray_app_t *app) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return;
    }

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--vid") == 0 && i + 1 < argc) {
            uint16_t v = 0U;
            if (kc_parse_u16_w(argv[++i], &v)) {
                app->target_vid = v;
                app->cli_target_override = true;
            }
        } else if (wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc) {
            uint16_t p = 0U;
            if (kc_parse_u16_w(argv[++i], &p)) {
                app->target_pid = p;
                app->cli_target_override = true;
            }
        } else if (wcscmp(argv[i], L"--interface") == 0 && i + 1 < argc) {
            int iface = -1;
            if (kc_parse_int_range_w(argv[++i], 0, 255, &iface)) {
                app->target_interface = iface;
                app->cli_target_override = true;
            }
        }
    }
    LocalFree(argv);
}

static const wchar_t *kc_device_label(const kc_tray_app_t *app) {
    if (app->device_product[0] != L'\0' && wcscmp(app->device_product, L"?") != 0) {
        return app->device_product;
    }
    return L"Keychron mouse";
}

static uint64_t kc_unix_now_sec(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    if (uli.QuadPart < 116444736000000000ULL) {
        return 0ULL;
    }
    return (uint64_t)((uli.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static void kc_refresh_host_power(kc_tray_app_t *app) {
    if (!app) {
        return;
    }
    SYSTEM_POWER_STATUS sps;
    if (!GetSystemPowerStatus(&sps)) {
        app->has_sys_battery = false;
        app->on_laptop_battery = false;
        app->laptop_pct = 100U;
        return;
    }
    bool no_batt = (sps.BatteryFlag == 255U) || ((sps.BatteryFlag & 128U) != 0U);
    app->has_sys_battery = !no_batt;
    app->on_laptop_battery = !no_batt && (sps.ACLineStatus == 0U);
    app->laptop_pct = (sps.BatteryLifePercent <= 100U) ? (uint8_t)sps.BatteryLifePercent : 100U;
}

static kc_icon_tone_t kc_visual_to_tone(kc_battery_visual_t vis) {
    switch (vis) {
        case KC_VIS_LOW:          return KC_TONE_LOW;
        case KC_VIS_CHARGING:     return KC_TONE_CHARGING;
        case KC_VIS_STALE:        return KC_TONE_STALE;
        case KC_VIS_CHARGE_SOON:  return KC_TONE_CHARGE_SOON;
        case KC_VIS_CHARGE_LIMIT: return KC_TONE_CHARGE_LIMIT;
        default:                  return KC_TONE_NORMAL;
    }
}

typedef enum kc_toast_kind {
    KC_TOAST_NONE = 0,
    KC_TOAST_CAL_COMPLETE,
    KC_TOAST_LOW,
    KC_TOAST_CHARGE_LIMIT,
    KC_TOAST_LAPTOP_GUARD,
    KC_TOAST_CHARGE_SOON,
    KC_TOAST_FULL,
    KC_TOAST_CAL_DUE
} kc_toast_kind_t;

static void kc_rollback_toast(kc_tray_app_t *app, kc_toast_kind_t toast) {
    switch (toast) {
        case KC_TOAST_CAL_COMPLETE:
            app->prot_rt.pending_cal_complete = true;
            break;
        case KC_TOAST_LOW:
            app->poll_state.low_battery_notified = false;
            break;
        case KC_TOAST_CHARGE_LIMIT:
            app->prot_rt.charge_limit_notified = false;
            break;
        case KC_TOAST_LAPTOP_GUARD:
            app->prot_rt.laptop_guard_notified = false;
            break;
        case KC_TOAST_CHARGE_SOON:
            app->prot_rt.charge_soon_notified = false;
            break;
        case KC_TOAST_FULL:
            app->poll_state.full_battery_notified = false;
            break;
        case KC_TOAST_CAL_DUE:
            app->prot_rt.cal_due_notified = false;
            break;
        default:
            break;
    }
}

static void kc_fill_toast_info(const kc_tray_app_t *app,
                               kc_toast_kind_t toast,
                               uint64_t now_unix,
                               NOTIFYICONDATAW *nid) {
    const wchar_t *name = kc_device_label(app);
    uint8_t min_p = 0U;
    uint8_t max_p = 100U;
    kc_protector_bounds(app->prot_persist.mode, &min_p, &max_p);

    if (toast == KC_TOAST_CAL_COMPLETE) {
        nid->dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        wcsncpy(nid->szInfoTitle, L"Battery Calibration Complete", 63);
        if (app->prot_persist.mode != (uint8_t)KC_PROT_OFF) {
            _snwprintf(nid->szInfo, 255,
                       L"%ls reached 100%% and reset cycle counters. Unplug mouse (%u%% limit resumed).",
                       name, (unsigned)max_p);
        } else {
            _snwprintf(nid->szInfo, 255, L"%ls calibrated cleanly to 100%%.", name);
        }
    } else if (toast == KC_TOAST_LOW) {
        nid->dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;
        wcsncpy(nid->szInfoTitle, L"Keychron Mouse Battery Low", 63);
        _snwprintf(nid->szInfo, 255, L"%ls battery is at %u%%.",
                   name, (unsigned)app->poll_state.last_pct);
    } else if (toast == KC_TOAST_CHARGE_LIMIT) {
        nid->dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        if (app->on_laptop_battery) {
            _snwprintf(nid->szInfoTitle, 63, L"Unplug Mouse (%u%% Limit)", (unsigned)max_p);
            _snwprintf(nid->szInfo, 255,
                       L"%ls reached %u%%. Unplug USB cable to extend mouse battery lifespan and save laptop battery.",
                       name, (unsigned)app->poll_state.last_pct);
        } else {
            _snwprintf(nid->szInfoTitle, 63, L"Recommended %u%% Limit Reached", (unsigned)max_p);
            _snwprintf(nid->szInfo, 255,
                       L"%ls is at %u%%. Unplug USB cable to maximize battery lifespan.",
                       name, (unsigned)app->poll_state.last_pct);
        }
    } else if (toast == KC_TOAST_LAPTOP_GUARD) {
        nid->dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;
        wcsncpy(nid->szInfoTitle, L"Laptop Battery Guard", 63);
        _snwprintf(nid->szInfo, 255,
                   L"Laptop battery is at %u%% while %ls is already %u%%. Consider unplugging mouse cable.",
                   (unsigned)app->laptop_pct, name, (unsigned)app->poll_state.last_pct);
    } else if (toast == KC_TOAST_CHARGE_SOON) {
        bool cal_due = kc_protector_is_cal_due(&app->prot_persist, now_unix, NULL, NULL);
        nid->dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        wcsncpy(nid->szInfoTitle, L"Good Time to Charge Mouse", 63);
        if (cal_due) {
            _snwprintf(nid->szInfo, 255,
                       L"%ls is at %u%% (recommended >=%u%%). Periodic 100%% calibration is also due.",
                       name, (unsigned)app->poll_state.last_pct, (unsigned)min_p);
        } else {
            _snwprintf(nid->szInfo, 255,
                       L"%ls is at %u%% (recommended charge point <=%u%%).",
                       name, (unsigned)app->poll_state.last_pct, (unsigned)min_p);
        }
    } else if (toast == KC_TOAST_FULL) {
        nid->dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        wcsncpy(nid->szInfoTitle, L"Keychron Mouse Charged", 63);
        _snwprintf(nid->szInfo, 255, L"%ls battery is fully charged (100%%).", name);
    } else if (toast == KC_TOAST_CAL_DUE) {
        uint32_t days = 0U, cyc_x10 = 0U;
        kc_protector_is_cal_due(&app->prot_persist, now_unix, &days, &cyc_x10);
        nid->dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        wcsncpy(nid->szInfoTitle, L"Battery Calibration Recommended", 63);
        _snwprintf(nid->szInfo, 255,
                   L"%ls: %u.%u cycles / %u days since last 100%% calibration. Enable 'Calibrate to 100%%' in Battery Protector next charge.",
                   name, (unsigned)(cyc_x10 / 10U), (unsigned)(cyc_x10 % 10U), (unsigned)days);
    }
    nid->szInfoTitle[63] = L'\0';
    nid->szInfo[255] = L'\0';
}

static void kc_format_status_line(const kc_tray_app_t *app, wchar_t out_line[128]) {
    const wchar_t *name = kc_device_label(app);
    const wchar_t *link = kc_hid_link_label_w(app->hid_session.active_link);

    if (app->poll_state.has_reading) {
        if (app->poll_state.consecutive_failures == 0U) {
            wchar_t detail[96] = L"";
            size_t pos = 0U;
            if (link[0] != L'\0') {
                int n = _snwprintf(detail + pos, 96U - pos, L"%ls", link);
                if (n > 0) pos += (size_t)n;
            }
            uint8_t min_p = 0U, max_p = 100U;
            kc_protector_bounds(app->prot_persist.mode, &min_p, &max_p);
            if (app->poll_state.last_charging && pos < 95U) {
                int n = 0;
                if (app->prot_persist.mode != (uint8_t)KC_PROT_OFF && app->prot_persist.calibrating) {
                    n = _snwprintf(detail + pos, 96U - pos, L"%lscal -> 100%%", pos > 0U ? L", " : L"");
                } else if (app->prot_persist.mode != (uint8_t)KC_PROT_OFF && app->poll_state.last_pct >= max_p) {
                    n = _snwprintf(detail + pos, 96U - pos, L"%ls%ls",
                                   pos > 0U ? L", " : L"",
                                   app->on_laptop_battery ? L"unplug (laptop batt)" : L"unplug (limit)");
                } else {
                    n = _snwprintf(detail + pos, 96U - pos, L"%lscharging", pos > 0U ? L", " : L"");
                }
                if (n > 0) pos += (size_t)n;
            } else if (!app->poll_state.last_charging &&
                       app->prot_persist.mode != (uint8_t)KC_PROT_OFF &&
                       min_p > 0U && app->poll_state.last_pct <= min_p &&
                       !(app->settings.low_alert_pct > 0U &&
                         app->poll_state.last_pct <= app->settings.low_alert_pct) &&
                       pos < 95U) {
                int n = _snwprintf(detail + pos, 96U - pos, L"%lscharge soon", pos > 0U ? L", " : L"");
                if (n > 0) pos += (size_t)n;
            }
            if (app->has_reading_time && pos < 95U) {
                _snwprintf(detail + pos, 96U - pos, L"%ls%02u:%02u",
                           pos > 0U ? L", " : L"",
                           (unsigned)app->last_reading_time.wHour,
                           (unsigned)app->last_reading_time.wMinute);
            }
            detail[95] = L'\0';
            if (detail[0] != L'\0') {
                _snwprintf(out_line, 128, L"%ls: %u%% (%ls)",
                           name, (unsigned)app->poll_state.last_pct, detail);
            } else {
                _snwprintf(out_line, 128, L"%ls: %u%%",
                           name, (unsigned)app->poll_state.last_pct);
            }
        } else if (app->last_status == KC_HID_TIMEOUT) {
            if (app->has_reading_time) {
                _snwprintf(out_line, 128, L"%ls: %u%% (asleep, last %02u:%02u)",
                           name, (unsigned)app->poll_state.last_pct,
                           (unsigned)app->last_reading_time.wHour,
                           (unsigned)app->last_reading_time.wMinute);
            } else {
                _snwprintf(out_line, 128, L"%ls: %u%% (asleep)",
                           name, (unsigned)app->poll_state.last_pct);
            }
        } else {
            _snwprintf(out_line, 128, L"%ls: %u%% (%ls)",
                       name, (unsigned)app->poll_state.last_pct,
                       kc_hid_status_text_w(app->last_status));
        }
    } else if (app->poll_state.consecutive_failures > 0U) {
        _snwprintf(out_line, 128, L"%ls: %ls", name, kc_hid_status_text_w(app->last_status));
    } else {
        _snwprintf(out_line, 128, L"%ls: checking...", name);
    }
    out_line[127] = L'\0';
}

static void kc_build_display_state(kc_tray_app_t *app,
                                   uint64_t now_unix,
                                   char out_glyph[8],
                                   wchar_t out_tip[128],
                                   kc_icon_tone_t *out_tone,
                                   kc_toast_kind_t *out_toast) {
    *out_tone = KC_TONE_NORMAL;
    *out_toast = KC_TOAST_NONE;

    if (app->poll_state.has_reading) {
        bool is_sleeping = (app->poll_state.consecutive_failures > 0U &&
                            app->last_status == KC_HID_TIMEOUT);
        bool err_glyph = !is_sleeping && kc_should_show_error_glyph(&app->poll_state);
        if (err_glyph) {
            strncpy(out_glyph, "!", 8);
        } else {
            snprintf(out_glyph, 8, "%u", (unsigned)app->poll_state.last_pct);
            kc_battery_visual_t vis = kc_compute_battery_visual(
                &app->poll_state, &app->prot_persist, app->settings.low_alert_pct);
            *out_tone = kc_visual_to_tone(vis);
        }

        if (app->poll_state.consecutive_failures == 0U) {
            bool cal_done = app->prot_rt.pending_cal_complete;
            if (cal_done) {
                app->prot_rt.pending_cal_complete = false;
                *out_toast = KC_TOAST_CAL_COMPLETE;
            }
            if (kc_check_low_battery_alert(&app->poll_state, app->settings.low_alert_pct) &&
                *out_toast == KC_TOAST_NONE) {
                *out_toast = KC_TOAST_LOW;
            }
            if (kc_check_charge_limit_alert(&app->poll_state, &app->prot_persist, &app->prot_rt) &&
                *out_toast == KC_TOAST_NONE) {
                *out_toast = KC_TOAST_CHARGE_LIMIT;
            }
            if (kc_check_laptop_guard_alert(&app->poll_state, &app->prot_persist, &app->prot_rt,
                                            app->has_sys_battery, app->on_laptop_battery, app->laptop_pct) &&
                *out_toast == KC_TOAST_NONE) {
                *out_toast = KC_TOAST_LAPTOP_GUARD;
            }
            if (kc_check_charge_soon_alert(&app->poll_state, &app->prot_persist, &app->prot_rt,
                                           app->settings.low_alert_pct) &&
                *out_toast == KC_TOAST_NONE) {
                *out_toast = KC_TOAST_CHARGE_SOON;
            }
            if (!cal_done && kc_check_full_battery_alert(&app->poll_state, app->settings.notify_full) &&
                *out_toast == KC_TOAST_NONE) {
                *out_toast = KC_TOAST_FULL;
            }
            if (*out_toast == KC_TOAST_NONE &&
                kc_check_cal_due_alert(&app->poll_state, &app->prot_persist, &app->prot_rt, now_unix)) {
                *out_toast = KC_TOAST_CAL_DUE;
            }
        }
    } else if (app->poll_state.consecutive_failures > 0U) {
        strncpy(out_glyph, "!", 8);
    } else {
        strncpy(out_glyph, "--", 8);
    }

    kc_format_status_line(app, out_tip);
    out_glyph[7] = '\0';
}

static void kc_sync_tray_icon(kc_tray_app_t *app, bool force_add) {
    char glyph[8];
    wchar_t tip[128];
    kc_icon_tone_t tone = KC_TONE_NORMAL;
    kc_toast_kind_t toast = KC_TOAST_NONE;
    uint64_t now_unix = kc_unix_now_sec();

    kc_refresh_host_power(app);
    kc_build_display_state(app, now_unix, glyph, tip, &tone, &toast);
    bool want_toast = (toast != KC_TOAST_NONE);

    HICON hicon = kc_icon_cache_get(
        &app->icon_cache, glyph, app->icon_size, app->light_taskbar, tone);
    if (!hicon) {
        kc_rollback_toast(app, toast);
        return;
    }

    bool icon_changed = (hicon != app->last_hicon) ||
                        (tone != app->last_tone) ||
                        (strcmp(glyph, app->last_glyph) != 0);
    bool tip_changed = (wcscmp(tip, app->last_tip) != 0);

    if (!force_add && app->tray_added && !icon_changed && !tip_changed && !want_toast) {
        return;
    }

    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = app->hwnd;
    nid.uID = TRAY_ICON_UID;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = hicon;
    wcsncpy(nid.szTip, tip, 127);
    nid.szTip[127] = L'\0';

    if (want_toast) {
        kc_fill_toast_info(app, toast, now_unix, &nid);
    }

    if (force_add || !app->tray_added) {
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        if (want_toast) {
            nid.uFlags |= NIF_INFO;
        }
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            app->tray_added = true;
        } else {
            kc_rollback_toast(app, toast);
            return;
        }
    } else {
        nid.uFlags = NIF_SHOWTIP;
        if (icon_changed) {
            nid.uFlags |= NIF_ICON;
        }
        if (tip_changed) {
            nid.uFlags |= NIF_TIP;
        }
        if (want_toast) {
            nid.uFlags |= NIF_INFO;
        }
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
            /* Explorer restart or shell notification reset dropped our icon; re-register. */
            app->tray_added = false;
            nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
            if (want_toast) {
                nid.uFlags |= NIF_INFO;
            }
            if (Shell_NotifyIconW(NIM_ADD, &nid)) {
                app->tray_added = true;
            } else {
                kc_rollback_toast(app, toast);
                return;
            }
        }
    }

    app->last_hicon = hicon;
    app->last_tone = tone;
    strncpy(app->last_glyph, glyph, sizeof(app->last_glyph) - 1);
    app->last_glyph[sizeof(app->last_glyph) - 1] = '\0';
    wcsncpy(app->last_tip, tip, 127);
    app->last_tip[127] = L'\0';
}

static void kc_show_custom_balloon(kc_tray_app_t *app,
                                   DWORD info_flags,
                                   const wchar_t *title,
                                   const wchar_t *body) {
    if (!app || !app->hwnd) {
        return;
    }
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = app->hwnd;
    nid.uID = TRAY_ICON_UID;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = app->last_hicon;
    wcsncpy(nid.szTip, app->last_tip[0] ? app->last_tip : L"Keychron Battery Tray", 127);
    nid.szTip[127] = L'\0';
    nid.dwInfoFlags = info_flags | NIIF_RESPECT_QUIET_TIME;
    wcsncpy(nid.szInfoTitle, title ? title : L"Keychron Battery Tray", 63);
    nid.szInfoTitle[63] = L'\0';
    wcsncpy(nid.szInfo, body ? body : L"", 255);
    nid.szInfo[255] = L'\0';

    if (app->tray_added) {
        nid.uFlags = NIF_INFO | NIF_SHOWTIP;
        if (Shell_NotifyIconW(NIM_MODIFY, &nid)) {
            return;
        }
        app->tray_added = false;
    }
    if (nid.hIcon) {
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_INFO;
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            app->tray_added = true;
        }
    }
}

static DWORD WINAPI kc_update_check_proc(LPVOID param) {
    kc_tray_app_t *app = (kc_tray_app_t *)param;
    kc_update_check_result_t res = kc_updater_check_latest(KC_APP_VERSION, app->stop_event);
    kc_update_check_result_t *heap_res =
        (kc_update_check_result_t *)calloc(1, sizeof(kc_update_check_result_t));
    if (!heap_res) {
        return 0;
    }
    *heap_res = res;
    if (!PostMessageW(app->hwnd, WM_APP_UPDATE_CHECK_DONE, 0, (LPARAM)heap_res)) {
        free(heap_res);
    }
    return 0;
}

static DWORD WINAPI kc_update_install_proc(LPVOID param) {
    kc_release_info_t *rel_in = (kc_release_info_t *)param;
    kc_release_info_t rel = *rel_in;
    free(rel_in);

    kc_update_install_result_t res = kc_updater_prepare_install(&rel, g_app.stop_event);
    kc_update_install_result_t *heap_res =
        (kc_update_install_result_t *)calloc(1, sizeof(kc_update_install_result_t));
    if (!heap_res) {
        if (res.ok) {
            kc_updater_cleanup_dir(res.work_dir);
        }
        return 0;
    }
    *heap_res = res;
    if (!PostMessageW(g_app.hwnd, WM_APP_UPDATE_INSTALL_DONE, 0, (LPARAM)heap_res)) {
        if (heap_res->ok) {
            kc_updater_cleanup_dir(heap_res->work_dir);
        }
        free(heap_res);
    }
    return 0;
}

static void kc_start_update_check(kc_tray_app_t *app) {
    if (!app || app->update_ui_state != KC_UPD_IDLE) {
        return;
    }
    if (app->update_thread) {
        CloseHandle(app->update_thread);
        app->update_thread = NULL;
    }
    app->update_ui_state = KC_UPD_CHECKING;
    HANDLE th = CreateThread(NULL, 0, kc_update_check_proc, app, 0, NULL);
    if (!th) {
        app->update_ui_state = KC_UPD_IDLE;
        kc_show_custom_balloon(app, NIIF_WARNING, L"Update Check Failed",
                               L"Could not start update background worker.");
        return;
    }
    app->update_thread = th;
}

static void kc_request_refresh(kc_tray_app_t *app, bool reset_cache) {
    EnterCriticalSection(&app->state_lock);
    if (reset_cache) {
        kc_hid_session_invalidate(&app->hid_session);
        app->session_epoch++;
    }
    if (app->refresh_in_flight) {
        app->pending_refresh = true;
    } else {
        SetEvent(app->wake_event);
    }
    LeaveCriticalSection(&app->state_lock);
}

static DWORD WINAPI kc_worker_proc(LPVOID param) {
    kc_tray_app_t *app = (kc_tray_app_t *)param;
    HANDLE waits[2] = {app->stop_event, app->wake_event};

    while (WaitForSingleObject(app->stop_event, 0) != WAIT_OBJECT_0) {
        EnterCriticalSection(&app->state_lock);
        ResetEvent(app->wake_event);
        app->refresh_in_flight = true;
        app->pending_refresh = false;
        uint32_t start_epoch = app->session_epoch;
        kc_hid_session_t local_session = app->hid_session;
        uint16_t vid = app->target_vid;
        uint16_t pid = app->target_pid;
        int iface = app->target_interface;
        bool in_backoff = (app->poll_state.consecutive_failures > 0U);
        LeaveCriticalSection(&app->state_lock);

        uint32_t retries = in_backoff ? 1U : 2U;
        uint32_t timeout_ms = in_backoff ? 1000U : 1500U;

        uint8_t pct = 0;
        bool charging = false;
        kc_hid_status_t st = kc_hid_query_battery(
            &local_session, vid, pid, iface, retries, timeout_ms,
            app->stop_event, false, NULL, &pct, &charging);

        if (st == KC_HID_CANCELLED) {
            break;
        }

        EnterCriticalSection(&app->state_lock);
        if (app->session_epoch == start_epoch) {
            app->hid_session = local_session;
            if (local_session.has_info) {
                wcsncpy(app->device_product, local_session.cached_info.product, 127);
                app->device_product[127] = L'\0';
            }
        } else {
            kc_hid_session_invalidate(&app->hid_session);
        }
        app->refresh_in_flight = false;
        bool rerun_now = app->pending_refresh;
        app->pending_refresh = false;
        bool epoch_match = (app->session_epoch == start_epoch);
        bool prot_dirty = false;
        kc_protector_persist_t prot_copy;
        memset(&prot_copy, 0, sizeof(prot_copy));
        if (epoch_match) {
            if (st == KC_HID_OK) {
                kc_poll_state_record_success(&app->poll_state, pct, charging, GetTickCount64());
                bool cal_completed = false;
                prot_dirty = kc_protector_record_sample(
                    &app->prot_persist, &app->prot_rt, pct, charging, kc_unix_now_sec(),
                    &cal_completed);
                if (cal_completed) {
                    app->poll_state.full_battery_notified = true;
                }
                if (prot_dirty) {
                    prot_copy = app->prot_persist;
                }
            } else {
                kc_poll_state_record_failure(&app->poll_state);
            }
        }
        uint32_t normal_ms = app->settings.poll_minutes * 60U * 1000U;
        uint32_t wait_ms = kc_next_wait_ms_for_wake(
            &app->poll_state, normal_ms, app->raw_watch_armed);
        LeaveCriticalSection(&app->state_lock);

        if (prot_dirty) {
            kc_protector_save(&prot_copy);
        }

        if (epoch_match) {
            PostMessageW(app->hwnd, WM_APP_POLL_RESULT, (WPARAM)st, (LPARAM)pct);
        }

        if (rerun_now) {
            continue;
        }
        DWORD wres = WaitForMultipleObjects(2, waits, FALSE, wait_ms);
        if (wres == WAIT_OBJECT_0) {
            break;
        }
    }
    return 0;
}

static UINT kc_poll_menu_id(uint32_t poll_minutes) {
    switch (poll_minutes) {
        case 5U:  return IDM_POLL_5;
        case 15U: return IDM_POLL_15;
        case 60U: return IDM_POLL_60;
        default:  return IDM_POLL_30;
    }
}

static UINT kc_alert_menu_id(uint8_t low_pct) {
    switch (low_pct) {
        case 0U:  return IDM_ALERT_OFF;
        case 10U: return IDM_ALERT_10;
        case 20U: return IDM_ALERT_20;
        default:  return IDM_ALERT_15;
    }
}

typedef struct kc_menu_dev_entry {
    uint16_t pid;
    int interface_number;
} kc_menu_dev_entry_t;

static void kc_populate_device_submenu(kc_tray_app_t *app,
                                       HMENU dev_sub,
                                       kc_menu_dev_entry_t out_devs[KC_MENU_DEV_MAX],
                                       size_t *out_count) {
    *out_count = 0U;

    EnterCriticalSection(&app->state_lock);
    uint16_t vid = app->target_vid;
    uint16_t cur_pid = app->target_pid;
    int cur_iface = app->target_interface;
    bool cli_override = app->cli_target_override;
    bool has_cached = app->hid_session.has_info;
    uint16_t cached_pid = app->hid_session.cached_info.pid;
    int cached_iface = app->hid_session.cached_info.interface_number;
    wchar_t cached_name[128];
    wcsncpy(cached_name, app->device_product, 127);
    cached_name[127] = L'\0';
    LeaveCriticalSection(&app->state_lock);

    AppendMenuW(dev_sub, MF_STRING, IDM_DEV_AUTO, L"Auto-detect");

    kc_hid_device_info_t list[32];
    size_t found = kc_hid_list_devices(vid, list, sizeof(list) / sizeof(list[0]));
    size_t limit = found < (sizeof(list) / sizeof(list[0])) ? found : (sizeof(list) / sizeof(list[0]));

    UINT checked_id = IDM_DEV_AUTO;
    bool matched_cur = (cur_pid == 0U && cur_iface < 0);

    for (size_t i = 0; i < limit && *out_count < KC_MENU_DEV_MAX; ++i) {
        kc_hid_candidate_t cand = {
            .vid = list[i].vid,
            .pid = list[i].pid,
            .usage_page = list[i].usage_page,
            .usage = list[i].usage,
            .interface_number = list[i].interface_number,
        };
        if (kc_score_candidate(&cand, vid, 0U, -1) < 0) {
            continue;
        }

        bool dup = false;
        for (size_t j = 0; j < *out_count; ++j) {
            if (out_devs[j].pid == list[i].pid &&
                out_devs[j].interface_number == list[i].interface_number) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        const wchar_t *base_name = (list[i].product[0] != L'\0' && wcscmp(list[i].product, L"?") != 0)
            ? list[i].product
            : L"Keychron HID";
        if (has_cached && cached_name[0] != L'\0' &&
            list[i].pid == cached_pid && list[i].interface_number == cached_iface) {
            base_name = cached_name;
        }

        wchar_t label[160];
        if (list[i].interface_number >= 0) {
            _snwprintf(label, 160, L"%ls (0x%04X, MI_%02d)",
                       base_name, (unsigned)list[i].pid, list[i].interface_number);
        } else {
            _snwprintf(label, 160, L"%ls (0x%04X)", base_name, (unsigned)list[i].pid);
        }
        label[159] = L'\0';

        UINT item_id = IDM_DEV_AUTO + 1U + (UINT)(*out_count);
        out_devs[*out_count].pid = list[i].pid;
        out_devs[*out_count].interface_number = list[i].interface_number;
        (*out_count)++;
        AppendMenuW(dev_sub, MF_STRING, item_id, label);

        if ((cur_pid == 0U || list[i].pid == cur_pid) &&
            (cur_iface < 0 || list[i].interface_number == cur_iface) &&
            (cur_pid != 0U || cur_iface >= 0) &&
            !matched_cur) {
            checked_id = item_id;
            matched_cur = true;
        }
    }

    if (!matched_cur && (cur_pid != 0U || cur_iface >= 0) && *out_count < KC_MENU_DEV_MAX) {
        wchar_t offline_label[160];
        if (cur_pid != 0U && cur_iface >= 0) {
            _snwprintf(offline_label, 160, L"Saved device (0x%04X, MI_%02d) [offline]",
                       (unsigned)cur_pid, cur_iface);
        } else if (cur_pid != 0U) {
            _snwprintf(offline_label, 160, L"Saved device (0x%04X) [offline]", (unsigned)cur_pid);
        } else {
            _snwprintf(offline_label, 160, L"Saved interface MI_%02d [offline]", cur_iface);
        }
        offline_label[159] = L'\0';
        UINT item_id = IDM_DEV_AUTO + 1U + (UINT)(*out_count);
        out_devs[*out_count].pid = cur_pid;
        out_devs[*out_count].interface_number = cur_iface;
        (*out_count)++;
        AppendMenuW(dev_sub, MF_STRING, item_id, offline_label);
        checked_id = item_id;
    }

    CheckMenuRadioItem(dev_sub, IDM_DEV_AUTO, IDM_DEV_AUTO + (UINT)(*out_count), checked_id, MF_BYCOMMAND);

    if (cli_override) {
        for (UINT id = IDM_DEV_AUTO; id <= IDM_DEV_AUTO + (UINT)(*out_count); ++id) {
            EnableMenuItem(dev_sub, id, MF_BYCOMMAND | MF_GRAYED);
        }
        AppendMenuW(dev_sub, MF_SEPARATOR, 0, NULL);
        AppendMenuW(dev_sub, MF_STRING | MF_GRAYED, 0, L"Overridden by CLI arguments");
    }
}

static void kc_copy_diagnostics(kc_tray_app_t *app) {
    char prod_utf8[256] = "";
    char prod_esc[512] = "";
    char status_utf8[128] = "";
    char status_esc[256] = "";
    char link_str[16] = "";

    EnterCriticalSection(&app->state_lock);
    wchar_t prod_w[128];
    wcsncpy(prod_w, kc_device_label(app), 127);
    prod_w[127] = L'\0';
    wchar_t status_w[128];
    wcsncpy(status_w, kc_hid_status_text_w(app->last_status), 127);
    status_w[127] = L'\0';
    uint16_t vid = app->hid_session.has_info ? app->hid_session.cached_info.vid : app->target_vid;
    uint16_t pid = app->hid_session.has_info ? app->hid_session.cached_info.pid : app->target_pid;
    int iface = app->hid_session.has_info ? app->hid_session.cached_info.interface_number : app->target_interface;
    uint16_t paired_pid = app->hid_session.paired_pid;
    kc_hid_link_t link = app->hid_session.active_link;
    bool has_reading = app->poll_state.has_reading;
    uint8_t pct = app->poll_state.last_pct;
    bool charging = app->poll_state.last_charging;
    uint32_t fails = app->poll_state.consecutive_failures;
    bool raw_armed = app->raw_watch_armed;
    uint32_t rate_x10 = 0U, hours_x10 = 0U;
    bool has_est = kc_estimate_drain(&app->poll_state, &rate_x10, &hours_x10);
    kc_protector_persist_t prot = app->prot_persist;
    bool has_sys_batt = app->has_sys_battery;
    bool on_batt = app->on_laptop_battery;
    uint8_t lap_pct = app->laptop_pct;
    LeaveCriticalSection(&app->state_lock);

    uint64_t now_unix = kc_unix_now_sec();
    uint32_t cal_days = 0U, cal_cyc_x10 = 0U;
    bool cal_due = kc_protector_is_cal_due(&prot, now_unix, &cal_days, &cal_cyc_x10);

    WideCharToMultiByte(CP_UTF8, 0, prod_w, -1, prod_utf8, sizeof(prod_utf8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, status_w, -1, status_utf8, sizeof(status_utf8), NULL, NULL);
    kc_json_escape(prod_utf8, prod_esc, sizeof(prod_esc));
    kc_json_escape(status_utf8, status_esc, sizeof(status_esc));
    const char *link_a = kc_hid_link_label_a(link);
    snprintf(link_str, sizeof(link_str), "%s", link_a[0] ? link_a : "unknown");

    char paired_buf[40] = "";
    if (paired_pid != 0U) {
        snprintf(paired_buf, sizeof(paired_buf), ",\"paired_pid\":\"0x%04x\"", (unsigned)paired_pid);
    }
    char est_buf[80] = "";
    if (has_est) {
        snprintf(est_buf, sizeof(est_buf),
                 ",\"drain_pct_per_hr\":%u.%u,\"est_hours\":%u.%u",
                 (unsigned)(rate_x10 / 10U), (unsigned)(rate_x10 % 10U),
                 (unsigned)(hours_x10 / 10U), (unsigned)(hours_x10 % 10U));
    }
    const char *prot_mode_s = (prot.mode == (uint8_t)KC_PROT_OFF) ? "off" :
                              (prot.mode == (uint8_t)KC_PROT_BALANCED) ? "balanced" : "longevity";
    char host_buf[64] = "";
    if (has_sys_batt) {
        snprintf(host_buf, sizeof(host_buf),
                 ",\"laptop_on_battery\":%s,\"laptop_pct\":%u",
                 on_batt ? "true" : "false", (unsigned)lap_pct);
    }

    char json[1536];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"vid\":\"0x%04x\",\"pid\":\"0x%04x\",\"interface\":%d,"
             "\"link\":\"%s\",\"product\":\"%s\"%s,\"has_reading\":%s,\"battery\":%u,"
             "\"charging\":%s,\"status\":\"%s\",\"failures\":%u,\"raw_wake_armed\":%s%s,"
             "\"protector_mode\":\"%s\",\"calibrating\":%s,\"cycles\":%u.%u,"
             "\"days_since_cal\":%u,\"cal_due\":%s%s}",
             KC_APP_VERSION, (unsigned)vid, (unsigned)pid, iface,
             link_str, prod_esc, paired_buf,
             has_reading ? "true" : "false", (unsigned)pct,
             charging ? "true" : "false", status_esc, (unsigned)fails,
             raw_armed ? "true" : "false", est_buf,
             prot_mode_s, prot.calibrating ? "true" : "false",
             (unsigned)(cal_cyc_x10 / 10U), (unsigned)(cal_cyc_x10 % 10U),
             (unsigned)cal_days, cal_due ? "true" : "false", host_buf);
    json[sizeof(json) - 1U] = '\0';

    int wchars = MultiByteToWideChar(CP_UTF8, 0, json, -1, NULL, 0);
    if (wchars <= 0) {
        return;
    }
    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wchars * sizeof(wchar_t));
    if (!hmem) {
        return;
    }
    wchar_t *dst = (wchar_t *)GlobalLock(hmem);
    if (!dst) {
        GlobalFree(hmem);
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, json, -1, dst, wchars);
    GlobalUnlock(hmem);

    if (!OpenClipboard(app->hwnd)) {
        GlobalFree(hmem);
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, hmem)) {
        CloseClipboard();
        GlobalFree(hmem);
        return;
    }
    CloseClipboard();
}

static UINT kc_protector_mode_menu_id(uint8_t mode) {
    if (mode == (uint8_t)KC_PROT_OFF) {
        return IDM_PROT_OFF;
    }
    if (mode == (uint8_t)KC_PROT_BALANCED) {
        return IDM_PROT_BALANCED;
    }
    return IDM_PROT_LONGEVITY;
}

static void kc_populate_protector_submenu(const kc_tray_app_t *app,
                                            HMENU prot_sub,
                                            const kc_protector_persist_t *prot,
                                            uint64_t now_unix,
                                            wchar_t out_parent_label[64]) {
    uint8_t min_p = 0U, max_p = 100U;
    kc_protector_bounds(prot->mode, &min_p, &max_p);
    uint32_t days = 0U, cyc_x10 = 0U;
    bool cal_due = kc_protector_is_cal_due(prot, now_unix, &days, &cyc_x10);
    bool at_limit = (prot->mode != (uint8_t)KC_PROT_OFF && !prot->calibrating &&
                     app->poll_state.has_reading && app->poll_state.consecutive_failures == 0U &&
                     app->poll_state.last_charging && app->poll_state.last_pct >= max_p);

    if (prot->calibrating) {
        wcsncpy(out_parent_label, L"Battery Protector (Calibrating -> 100%)", 63);
    } else if (at_limit) {
        _snwprintf(out_parent_label, 64, L"Battery Protector (%u%% cap - unplug!)", (unsigned)max_p);
    } else if (cal_due) {
        wcsncpy(out_parent_label, L"Battery Protector (Calibration due)", 63);
    } else if (prot->mode == (uint8_t)KC_PROT_OFF) {
        wcsncpy(out_parent_label, L"Battery Protector (Off)", 63);
    } else {
        _snwprintf(out_parent_label, 64, L"Battery Protector (%u%%-%u%%)", (unsigned)min_p, (unsigned)max_p);
    }
    out_parent_label[63] = L'\0';

    wchar_t info1[96];
    if (prot->mode == (uint8_t)KC_PROT_OFF) {
        wcsncpy(info1, L"Status: Protection off", 95);
    } else if (prot->calibrating) {
        wcsncpy(info1, L"Status: Calibrating (charging up to 100%)", 95);
    } else {
        _snwprintf(info1, 96, L"Active: Yellow <=%u%%, unplug >=%u%%",
                   (unsigned)min_p, (unsigned)max_p);
    }
    info1[95] = L'\0';
    AppendMenuW(prot_sub, MF_STRING | MF_GRAYED, IDM_PROT_INFO1, info1);

    wchar_t info2[96];
    _snwprintf(info2, 96, L"Cycles: %u.%u / %u (%u / %u days since cal.)",
               (unsigned)(cyc_x10 / 10U), (unsigned)(cyc_x10 % 10U),
               (unsigned)(KC_CAL_CYCLE_TARGET_PCT / 100U),
               (unsigned)days, (unsigned)KC_CAL_INTERVAL_DAYS);
    info2[95] = L'\0';
    AppendMenuW(prot_sub, MF_STRING | MF_GRAYED, IDM_PROT_INFO2, info2);

    if (app->has_sys_battery) {
        wchar_t info3[96];
        _snwprintf(info3, 96, L"Laptop power: %ls (%u%%)",
                   app->on_laptop_battery ? L"On battery" : L"AC plugged in",
                   (unsigned)app->laptop_pct);
        info3[95] = L'\0';
        AppendMenuW(prot_sub, MF_STRING | MF_GRAYED, IDM_PROT_INFO3, info3);
    }

    AppendMenuW(prot_sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(prot_sub, MF_STRING, IDM_PROT_LONGEVITY, L"Longevity (25% - 80%) [Recommended]");
    AppendMenuW(prot_sub, MF_STRING, IDM_PROT_BALANCED, L"Balanced (20% - 90%)");
    AppendMenuW(prot_sub, MF_STRING, IDM_PROT_OFF, L"Off");
    CheckMenuRadioItem(prot_sub, IDM_PROT_LONGEVITY, IDM_PROT_OFF,
                       kc_protector_mode_menu_id(prot->mode), MF_BYCOMMAND);

    AppendMenuW(prot_sub, MF_SEPARATOR, 0, NULL);
    UINT lguard_flags = MF_STRING | (prot->laptop_guard ? MF_CHECKED : MF_UNCHECKED);
    if (prot->mode == (uint8_t)KC_PROT_OFF) {
        lguard_flags |= MF_GRAYED;
    }
    AppendMenuW(prot_sub, lguard_flags, IDM_PROT_LAPTOP_GUARD,
                L"Laptop battery guard (warn on low laptop batt)");

    AppendMenuW(prot_sub, MF_SEPARATOR, 0, NULL);
    const wchar_t *cal_label = prot->calibrating
        ? L"Cancel 100% calibration"
        : (cal_due ? L"Calibrate battery to 100% (recommended)"
                   : L"Calibrate battery to 100% next charge");
    UINT cal_flags = MF_STRING;
    if (prot->mode == (uint8_t)KC_PROT_OFF) {
        cal_flags |= MF_GRAYED;
    } else if (prot->calibrating) {
        cal_flags |= MF_CHECKED;
    }
    AppendMenuW(prot_sub, cal_flags, IDM_PROT_CAL_TOGGLE, cal_label);
    wchar_t snooze_label[64];
    _snwprintf(snooze_label, 64, L"Snooze calibration reminder (%u days)",
               (unsigned)KC_CAL_SNOOZE_DAYS);
    snooze_label[63] = L'\0';
    AppendMenuW(prot_sub,
                MF_STRING | ((cal_due && !prot->calibrating) ? 0U : MF_GRAYED),
                IDM_PROT_CAL_SNOOZE,
                snooze_label);
    AppendMenuW(prot_sub, MF_STRING, IDM_PROT_CAL_RESET,
                L"Reset cycle counter (mark calibrated)");
}

static void kc_show_context_menu(kc_tray_app_t *app) {
    HMENU menu = CreatePopupMenu();
    HMENU dev_sub = CreatePopupMenu();
    HMENU prot_sub = CreatePopupMenu();
    HMENU poll_sub = CreatePopupMenu();
    HMENU alert_sub = CreatePopupMenu();
    if (!menu || !dev_sub || !prot_sub || !poll_sub || !alert_sub) {
        if (dev_sub) DestroyMenu(dev_sub);
        if (prot_sub) DestroyMenu(prot_sub);
        if (poll_sub) DestroyMenu(poll_sub);
        if (alert_sub) DestroyMenu(alert_sub);
        if (menu) DestroyMenu(menu);
        return;
    }

    kc_menu_dev_entry_t devs[KC_MENU_DEV_MAX];
    size_t dev_count = 0U;
    kc_populate_device_submenu(app, dev_sub, devs, &dev_count);

    wchar_t status_line[128];
    wchar_t est_line[96] = L"";
    wchar_t prot_title[64] = L"Battery Protector";
    uint32_t poll_minutes = 30U;
    uint8_t low_pct = 15U;
    bool notify_full = true;
    bool has_est = false;
    uint64_t now_unix = kc_unix_now_sec();

    EnterCriticalSection(&app->state_lock);
    kc_refresh_host_power(app);
    poll_minutes = app->settings.poll_minutes;
    low_pct = app->settings.low_alert_pct;
    notify_full = app->settings.notify_full;
    kc_format_status_line(app, status_line);
    kc_populate_protector_submenu(app, prot_sub, &app->prot_persist, now_unix, prot_title);
    uint32_t rate_x10 = 0U, hours_x10 = 0U;
    if (kc_estimate_drain(&app->poll_state, &rate_x10, &hours_x10)) {
        has_est = true;
        if (hours_x10 >= 100U) {
            _snwprintf(est_line, 96, L"Est. ~%uh left (%u.%u%%/h active)",
                       (unsigned)((hours_x10 + 5U) / 10U),
                       (unsigned)(rate_x10 / 10U), (unsigned)(rate_x10 % 10U));
        } else {
            _snwprintf(est_line, 96, L"Est. ~%u.%uh left (%u.%u%%/h active)",
                       (unsigned)(hours_x10 / 10U), (unsigned)(hours_x10 % 10U),
                       (unsigned)(rate_x10 / 10U), (unsigned)(rate_x10 % 10U));
        }
        est_line[95] = L'\0';
    }
    LeaveCriticalSection(&app->state_lock);

    AppendMenuW(poll_sub, MF_STRING, IDM_POLL_5, L"Every 5 minutes");
    AppendMenuW(poll_sub, MF_STRING, IDM_POLL_15, L"Every 15 minutes");
    AppendMenuW(poll_sub, MF_STRING, IDM_POLL_30, L"Every 30 minutes");
    AppendMenuW(poll_sub, MF_STRING, IDM_POLL_60, L"Every 60 minutes");
    CheckMenuRadioItem(poll_sub, IDM_POLL_5, IDM_POLL_60, kc_poll_menu_id(poll_minutes), MF_BYCOMMAND);

    AppendMenuW(alert_sub, MF_STRING, IDM_ALERT_OFF, L"Low: Off");
    AppendMenuW(alert_sub, MF_STRING, IDM_ALERT_10, L"Low: Below 10%");
    AppendMenuW(alert_sub, MF_STRING, IDM_ALERT_15, L"Low: Below 15%");
    AppendMenuW(alert_sub, MF_STRING, IDM_ALERT_20, L"Low: Below 20%");
    CheckMenuRadioItem(alert_sub, IDM_ALERT_OFF, IDM_ALERT_20, kc_alert_menu_id(low_pct), MF_BYCOMMAND);
    AppendMenuW(alert_sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(alert_sub,
                MF_STRING | (notify_full ? MF_CHECKED : MF_UNCHECKED),
                IDM_ALERT_FULL,
                L"Notify when full (100%)");

    AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_STATUS, status_line);
    if (has_est) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_STATUS_EST, est_line);
    }
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"Refresh now");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)prot_sub, prot_title);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)dev_sub, L"Device");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)poll_sub, L"Poll interval");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)alert_sub, L"Alerts");
    AppendMenuW(menu,
                MF_STRING | (kc_autostart_is_enabled() ? MF_CHECKED : MF_UNCHECKED),
                IDM_AUTOSTART,
                L"Run at Windows startup");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_COPY_DIAG, L"Copy diagnostics");
    if (app->update_ui_state == KC_UPD_CHECKING) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_CHECK_UPDATES, L"Checking for updates...");
    } else if (app->update_ui_state == KC_UPD_INSTALLING) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_CHECK_UPDATES, L"Downloading update...");
    } else {
        AppendMenuW(menu, MF_STRING, IDM_CHECK_UPDATES, L"Check for updates...");
    }
    AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_VERSION, L"Keychron Battery Tray v" KC_WIDE(KC_APP_VERSION));
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(app->hwnd);
    app->menu_open = true;
    UINT cmd = (UINT)TrackPopupMenuEx(
        menu,
        TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x,
        pt.y,
        app->hwnd,
        NULL);
    app->menu_open = false;
    PostMessageW(app->hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd >= IDM_DEV_AUTO && cmd <= IDM_DEV_AUTO + (UINT)dev_count) {
        if (!app->cli_target_override) {
            uint16_t new_pid = 0U;
            int new_iface = -1;
            if (cmd > IDM_DEV_AUTO) {
                size_t idx = (size_t)(cmd - IDM_DEV_AUTO - 1U);
                new_pid = devs[idx].pid;
                new_iface = devs[idx].interface_number;
            }
            EnterCriticalSection(&app->state_lock);
            app->target_pid = new_pid;
            app->target_interface = new_iface;
            app->settings.target_pid = new_pid;
            app->settings.target_interface = (int32_t)new_iface;
            kc_settings_save(&app->settings);
            app->device_product[0] = L'\0';
            app->has_reading_time = false;
            kc_poll_state_init(&app->poll_state);
            kc_protector_runtime_reset_session(&app->prot_rt);
            app->raw_cache_count = 0U;
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            kc_set_raw_mouse_watch(app, false);
            if (kc_autostart_is_enabled()) {
                kc_autostart_set(true, app->target_vid, app->target_pid, app->target_interface);
            }
            kc_request_refresh(app, true);
        }
        return;
    }

    switch (cmd) {
        case IDM_PROT_LONGEVITY:
        case IDM_PROT_BALANCED:
        case IDM_PROT_OFF: {
            uint8_t new_mode = (cmd == IDM_PROT_OFF) ? (uint8_t)KC_PROT_OFF :
                               (cmd == IDM_PROT_BALANCED) ? (uint8_t)KC_PROT_BALANCED :
                               (uint8_t)KC_PROT_LONGEVITY;
            EnterCriticalSection(&app->state_lock);
            app->prot_persist.mode = new_mode;
            if (new_mode == (uint8_t)KC_PROT_OFF) {
                app->prot_persist.calibrating = false;
            }
            app->prot_rt.charge_soon_notified = false;
            app->prot_rt.charge_limit_notified = false;
            kc_protector_save(&app->prot_persist);
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            break;
        }
        case IDM_PROT_LAPTOP_GUARD:
            EnterCriticalSection(&app->state_lock);
            app->prot_persist.laptop_guard = !app->prot_persist.laptop_guard;
            app->prot_rt.laptop_guard_notified = false;
            kc_protector_save(&app->prot_persist);
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            break;
        case IDM_PROT_CAL_TOGGLE:
            EnterCriticalSection(&app->state_lock);
            kc_protector_set_calibrating(&app->prot_persist, &app->prot_rt,
                                         !app->prot_persist.calibrating);
            kc_protector_save(&app->prot_persist);
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            break;
        case IDM_PROT_CAL_SNOOZE:
            EnterCriticalSection(&app->state_lock);
            kc_protector_snooze_calibration(&app->prot_persist, &app->prot_rt, kc_unix_now_sec());
            kc_protector_save(&app->prot_persist);
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            break;
        case IDM_PROT_CAL_RESET:
            EnterCriticalSection(&app->state_lock);
            kc_protector_reset_calibration(&app->prot_persist, &app->prot_rt, kc_unix_now_sec());
            kc_protector_save(&app->prot_persist);
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            break;
        case IDM_REFRESH:
            EnterCriticalSection(&app->state_lock);
            app->hid_session.next_fallback_probe_ms = 0ULL;
            LeaveCriticalSection(&app->state_lock);
            kc_request_refresh(app, false);
            break;
        case IDM_COPY_DIAG:
            kc_copy_diagnostics(app);
            break;
        case IDM_CHECK_UPDATES:
            kc_start_update_check(app);
            break;
        case IDM_AUTOSTART:
            kc_autostart_set(!kc_autostart_is_enabled(),
                             app->target_vid, app->target_pid, app->target_interface);
            break;
        case IDM_POLL_5:
        case IDM_POLL_15:
        case IDM_POLL_30:
        case IDM_POLL_60: {
            uint32_t mins = (cmd == IDM_POLL_5) ? 5U :
                            (cmd == IDM_POLL_15) ? 15U :
                            (cmd == IDM_POLL_60) ? 60U : 30U;
            EnterCriticalSection(&app->state_lock);
            app->settings.poll_minutes = mins;
            kc_settings_save(&app->settings);
            LeaveCriticalSection(&app->state_lock);
            kc_request_refresh(app, false);
            break;
        }
        case IDM_ALERT_OFF:
        case IDM_ALERT_10:
        case IDM_ALERT_15:
        case IDM_ALERT_20: {
            uint8_t pct = (cmd == IDM_ALERT_OFF) ? 0U :
                          (cmd == IDM_ALERT_10) ? 10U :
                          (cmd == IDM_ALERT_20) ? 20U : 15U;
            EnterCriticalSection(&app->state_lock);
            app->settings.low_alert_pct = pct;
            app->poll_state.low_battery_notified = false;
            kc_settings_save(&app->settings);
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            break;
        }
        case IDM_ALERT_FULL:
            EnterCriticalSection(&app->state_lock);
            app->settings.notify_full = !app->settings.notify_full;
            app->poll_state.full_battery_notified = false;
            kc_settings_save(&app->settings);
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            break;
        case IDM_QUIT:
            DestroyWindow(app->hwnd);
            break;
        default:
            break;
    }
}

static bool kc_is_relevant_hid_event(const kc_tray_app_t *app, LPARAM lparam) {
    const DEV_BROADCAST_HDR *hdr = (const DEV_BROADCAST_HDR *)lparam;
    if (!hdr || hdr->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE) {
        return false;
    }
    const DEV_BROADCAST_DEVICEINTERFACE_W *dbi =
        (const DEV_BROADCAST_DEVICEINTERFACE_W *)lparam;
    if (dbi->dbcc_size <= offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name)) {
        return true;
    }
    return kc_path_might_match_vid_w(dbi->dbcc_name, app->target_vid);
}

static void kc_set_raw_mouse_watch(kc_tray_app_t *app, bool enable) {
    if (!app || !app->hwnd) {
        return;
    }
    EnterCriticalSection(&app->state_lock);
    bool already = (app->raw_watch_armed == enable);
    LeaveCriticalSection(&app->state_lock);
    if (already) {
        return;
    }

    RAWINPUTDEVICE rid;
    memset(&rid, 0, sizeof(rid));
    rid.usUsagePage = 0x01U; /* HID_USAGE_PAGE_GENERIC */
    rid.usUsage = 0x02U;     /* HID_USAGE_GENERIC_MOUSE */
    rid.dwFlags = enable ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    rid.hwndTarget = enable ? app->hwnd : NULL;

    if (RegisterRawInputDevices(&rid, 1U, sizeof(rid))) {
        EnterCriticalSection(&app->state_lock);
        app->raw_watch_armed = enable;
        LeaveCriticalSection(&app->state_lock);
    }
}

static bool kc_raw_device_is_keychron(kc_tray_app_t *app, HANDLE hdev) {
    if (!hdev) {
        return false;
    }
    for (size_t i = 0; i < app->raw_cache_count; ++i) {
        if (app->raw_cache[i].hdevice == hdev) {
            return app->raw_cache[i].is_match;
        }
    }

    wchar_t dev_name[512];
    UINT chars = (UINT)(sizeof(dev_name) / sizeof(dev_name[0]));
    memset(dev_name, 0, sizeof(dev_name));
    bool matched = false;
    if (GetRawInputDeviceInfoW(hdev, RIDI_DEVICENAME, dev_name, &chars) != (UINT)-1) {
        matched = kc_raw_path_matches_target_w(dev_name, app->target_vid, app->target_pid);
    }

    if (app->raw_cache_count < KC_RAW_CACHE_CAP) {
        app->raw_cache[app->raw_cache_count].hdevice = hdev;
        app->raw_cache[app->raw_cache_count].is_match = matched;
        app->raw_cache_count++;
    }
    return matched;
}

static void kc_handle_raw_input(kc_tray_app_t *app, HRAWINPUT hraw) {
    if (!app->raw_watch_armed) {
        return;
    }
    RAWINPUTHEADER hdr;
    UINT sz = sizeof(hdr);
    if (GetRawInputData(hraw, RID_HEADER, &hdr, &sz, sizeof(hdr)) != sizeof(hdr)) {
        return;
    }
    if (hdr.dwType != RIM_TYPEMOUSE) {
        return;
    }
    if (kc_raw_device_is_keychron(app, hdr.hDevice)) {
        /* Disarm immediately so high-polling mice don't flood WM_INPUT once awake. */
        kc_set_raw_mouse_watch(app, false);
        SetTimer(app->hwnd, TIMER_INPUT_WAKE, 250U, NULL);
    }
}

static LRESULT CALLBACK kc_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    kc_tray_app_t *app = &g_app;

    if (msg != 0U && msg == app->wm_taskbar_created) {
        app->tray_added = false;
        EnterCriticalSection(&app->state_lock);
        kc_sync_tray_icon(app, true);
        LeaveCriticalSection(&app->state_lock);
        return 0;
    }

    switch (msg) {
        case WM_TRAY_CALLBACK:
            if (app->menu_open) {
                return 0;
            }
            if (lparam == WM_LBUTTONUP) {
                EnterCriticalSection(&app->state_lock);
                app->hid_session.next_fallback_probe_ms = 0ULL;
                LeaveCriticalSection(&app->state_lock);
                kc_request_refresh(app, false);
            } else if (lparam == WM_RBUTTONUP) {
                kc_show_context_menu(app);
            }
            return 0;

        case WM_APP_UPDATE_CHECK_DONE: {
            kc_update_check_result_t *heap_res = (kc_update_check_result_t *)lparam;
            if (app->update_thread) {
                CloseHandle(app->update_thread);
                app->update_thread = NULL;
            }
            app->update_ui_state = KC_UPD_IDLE;
            if (!heap_res) {
                return 0;
            }
            kc_update_check_result_t res = *heap_res;
            free(heap_res);

            if (!res.ok) {
                kc_show_custom_balloon(app, NIIF_WARNING, L"Update Check Failed", res.error);
                return 0;
            }
            if (!res.update_available) {
                kc_show_custom_balloon(
                    app, NIIF_INFO, L"Keychron Battery Tray",
                    L"You're up to date (v" KC_WIDE(KC_APP_VERSION) L").");
                return 0;
            }

            wchar_t ver_w[32] = L"";
            MultiByteToWideChar(CP_UTF8, 0, res.release.version_str, -1, ver_w, 32);
            wchar_t prompt[320];
            _snwprintf(prompt, 320,
                       L"Keychron Battery Tray v%ls is available (you have v%ls).\n\n"
                       L"Download, verify SHA-256, and update automatically?",
                       ver_w, KC_WIDE(KC_APP_VERSION));
            prompt[319] = L'\0';

            app->menu_open = true;
            int ans = MessageBoxW(hwnd, prompt, L"Keychron Battery Tray Update",
                                  MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND);
            app->menu_open = false;
            if (ans == IDYES) {
                kc_release_info_t *rel_arg =
                    (kc_release_info_t *)calloc(1, sizeof(kc_release_info_t));
                if (!rel_arg) {
                    kc_show_custom_balloon(app, NIIF_ERROR, L"Update Failed", L"Out of memory.");
                    return 0;
                }
                *rel_arg = res.release;
                app->update_ui_state = KC_UPD_INSTALLING;
                HANDLE th = CreateThread(NULL, 0, kc_update_install_proc, rel_arg, 0, NULL);
                if (!th) {
                    free(rel_arg);
                    app->update_ui_state = KC_UPD_IDLE;
                    kc_show_custom_balloon(app, NIIF_ERROR, L"Update Failed",
                                           L"Could not start download worker.");
                    return 0;
                }
                app->update_thread = th;
                kc_show_custom_balloon(app, NIIF_INFO, L"Downloading Update",
                                       L"Downloading and verifying SHA-256 checksum...");
            }
            return 0;
        }

        case WM_APP_UPDATE_INSTALL_DONE: {
            kc_update_install_result_t *heap_res = (kc_update_install_result_t *)lparam;
            if (app->update_thread) {
                CloseHandle(app->update_thread);
                app->update_thread = NULL;
            }
            app->update_ui_state = KC_UPD_IDLE;
            if (!heap_res) {
                return 0;
            }
            kc_update_install_result_t res = *heap_res;
            free(heap_res);

            if (!res.ok) {
                kc_show_custom_balloon(app, NIIF_ERROR, L"Update Failed", res.error);
                return 0;
            }
            if (!kc_updater_launch_script(res.script_path)) {
                kc_updater_cleanup_dir(res.work_dir);
                kc_show_custom_balloon(app, NIIF_ERROR, L"Update Failed",
                                       L"Could not launch update installer script.");
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_APP_POLL_RESULT: {
            bool arm_raw_watch = false;
            EnterCriticalSection(&app->state_lock);
            app->last_status = (kc_hid_status_t)wparam;
            if (app->last_status == KC_HID_OK) {
                GetLocalTime(&app->last_reading_time);
                app->has_reading_time = true;
            } else if (app->last_status != KC_HID_NOT_FOUND &&
                       app->poll_state.consecutive_failures > 0U) {
                arm_raw_watch = true;
            }
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            kc_set_raw_mouse_watch(app, arm_raw_watch);
            return 0;
        }

        case WM_INPUT:
            kc_handle_raw_input(app, (HRAWINPUT)lparam);
            return DefWindowProcW(hwnd, msg, wparam, lparam);

        case WM_POWERBROADCAST:
            if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
                SetTimer(hwnd, TIMER_RESUME_REFRESH, 1500U, NULL);
            } else if (wparam == PBT_POWERSETTINGCHANGE && lparam != 0) {
                const POWERBROADCAST_SETTING *pbs = (const POWERBROADCAST_SETTING *)lparam;
                if (IsEqualGUID(&pbs->PowerSetting, &KC_GUID_CONSOLE_DISPLAY_STATE) &&
                    pbs->DataLength >= sizeof(DWORD) &&
                    *(const DWORD *)pbs->Data != 0U) {
                    EnterCriticalSection(&app->state_lock);
                    bool stale = !app->poll_state.has_reading ||
                        ((GetTickCount64() - app->poll_state.last_success_ms) >= KC_DISPLAY_WAKE_COOLDOWN_MS);
                    LeaveCriticalSection(&app->state_lock);
                    if (stale) {
                        SetTimer(hwnd, TIMER_RESUME_REFRESH, 1500U, NULL);
                    }
                }
            }
            if (wparam == PBT_APMPOWERSTATUSCHANGE) {
                EnterCriticalSection(&app->state_lock);
                kc_refresh_host_power(app);
                kc_sync_tray_icon(app, false);
                LeaveCriticalSection(&app->state_lock);
            }
            return TRUE;

        case WM_DEVICECHANGE:
            if ((wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE) &&
                kc_is_relevant_hid_event(app, lparam)) {
                EnterCriticalSection(&app->state_lock);
                bool was_usb = (app->hid_session.active_link == KC_LINK_USB);
                app->raw_cache_count = 0U;
                LeaveCriticalSection(&app->state_lock);
                UINT delay_ms = (wparam == DBT_DEVICEREMOVECOMPLETE && was_usb) ? 1500U : 500U;
                SetTimer(hwnd, TIMER_USB_REFRESH, delay_ms, NULL);
            }
            return TRUE;

        case WM_TIMER:
            if (wparam == TIMER_RESUME_REFRESH ||
                wparam == TIMER_USB_REFRESH ||
                wparam == TIMER_INPUT_WAKE) {
                KillTimer(hwnd, wparam);
                kc_request_refresh(app, wparam == TIMER_USB_REFRESH);
            }
            return 0;

        case WM_SETTINGCHANGE:
            if (lparam != 0 && wcscmp((const wchar_t *)lparam, L"ImmersiveColorSet") != 0) {
                return 0;
            }
            EnterCriticalSection(&app->state_lock);
            app->icon_size = kc_query_tray_icon_size(hwnd);
            app->light_taskbar = kc_is_taskbar_light();
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            return 0;

        case WM_DPICHANGED: {
            UINT new_dpi = HIWORD(wparam);
            int sz = (new_dpi != 0U)
                ? GetSystemMetricsForDpi(SM_CXSMICON, new_dpi)
                : kc_query_tray_icon_size(hwnd);
            EnterCriticalSection(&app->state_lock);
            app->icon_size = (sz > 0) ? sz : 16;
            app->light_taskbar = kc_is_taskbar_light();
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            return 0;
        }

        case WM_DISPLAYCHANGE:
            EnterCriticalSection(&app->state_lock);
            app->icon_size = kc_query_tray_icon_size(hwnd);
            app->light_taskbar = kc_is_taskbar_light();
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            return 0;

        case WM_ENDSESSION:
            if (wparam) {
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_DESTROY:
            kc_set_raw_mouse_watch(app, false);
            if (app->tray_added) {
                NOTIFYICONDATAW nid;
                memset(&nid, 0, sizeof(nid));
                nid.cbSize = sizeof(nid);
                nid.hWnd = hwnd;
                nid.uID = TRAY_ICON_UID;
                Shell_NotifyIconW(NIM_DELETE, &nid);
                app->tray_added = false;
            }
            SetEvent(app->stop_event);
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE hprev, PWSTR cmdline, int show) {
    (void)hprev;
    (void)cmdline;
    (void)show;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    memset(&g_app, 0, sizeof(g_app));
    g_app.hinst = hinst;
    kc_settings_load(&g_app.settings);
    kc_protector_load(&g_app.prot_persist, 0ULL);
    kc_protector_runtime_init(&g_app.prot_rt);
    kc_refresh_host_power(&g_app);
    g_app.target_vid = KEYCHRON_VID;
    g_app.target_pid = g_app.settings.target_pid;
    g_app.target_interface = (int)g_app.settings.target_interface;
    kc_parse_tray_args(&g_app);

    g_app.mutex = CreateMutexW(NULL, FALSE, L"Local\\KeychronBatteryTrayMutex");
    if (!g_app.mutex) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_app.mutex);
        return 0;
    }

    InitializeCriticalSection(&g_app.state_lock);
    kc_hid_session_init(&g_app.hid_session);
    kc_poll_state_init(&g_app.poll_state);
    kc_icon_cache_init(&g_app.icon_cache);

    g_app.wake_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_app.stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_app.wake_event || !g_app.stop_event) {
        if (g_app.wake_event) CloseHandle(g_app.wake_event);
        if (g_app.stop_event) CloseHandle(g_app.stop_event);
        DeleteCriticalSection(&g_app.state_lock);
        CloseHandle(g_app.mutex);
        return 1;
    }

    g_app.wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = kc_wnd_proc;
    wc.hInstance = hinst;
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = (HICON)LoadImageW(
        hinst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"KeychronBatteryTrayWindow";

    if (!RegisterClassExW(&wc)) {
        CloseHandle(g_app.wake_event);
        CloseHandle(g_app.stop_event);
        DeleteCriticalSection(&g_app.state_lock);
        CloseHandle(g_app.mutex);
        return 1;
    }

    g_app.hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Keychron Battery Tray",
        WS_POPUP,
        0, 0, 0, 0,
        NULL,
        NULL,
        hinst,
        NULL);
    if (!g_app.hwnd) {
        UnregisterClassW(wc.lpszClassName, hinst);
        CloseHandle(g_app.wake_event);
        CloseHandle(g_app.stop_event);
        DeleteCriticalSection(&g_app.state_lock);
        CloseHandle(g_app.mutex);
        return 1;
    }

    ChangeWindowMessageFilterEx(g_app.hwnd, g_app.wm_taskbar_created, MSGFLT_ALLOW, NULL);

    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    DEV_BROADCAST_DEVICEINTERFACE_W dbi;
    memset(&dbi, 0, sizeof(dbi));
    dbi.dbcc_size = sizeof(dbi);
    dbi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    dbi.dbcc_classguid = hid_guid;
    g_app.dev_notify = RegisterDeviceNotificationW(g_app.hwnd, &dbi, DEVICE_NOTIFY_WINDOW_HANDLE);
    g_app.power_notify = RegisterPowerSettingNotification(
        g_app.hwnd, &KC_GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_app.icon_size = kc_query_tray_icon_size(g_app.hwnd);
    g_app.light_taskbar = kc_is_taskbar_light();
    kc_sync_tray_icon(&g_app, true);

    g_app.worker_thread = CreateThread(NULL, 0, kc_worker_proc, &g_app, 0, NULL);

    MSG msg;
    BOOL ret;
    while ((ret = GetMessageW(&msg, NULL, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetEvent(g_app.stop_event);
    if (g_app.update_thread) {
        WaitForSingleObject(g_app.update_thread, 2000);
        CloseHandle(g_app.update_thread);
    }
    if (g_app.worker_thread) {
        WaitForSingleObject(g_app.worker_thread, 2000);
        CloseHandle(g_app.worker_thread);
    }
    if (g_app.power_notify) {
        UnregisterPowerSettingNotification(g_app.power_notify);
    }
    if (g_app.dev_notify) {
        UnregisterDeviceNotification(g_app.dev_notify);
    }

    kc_icon_cache_destroy(&g_app.icon_cache);
    UnregisterClassW(wc.lpszClassName, hinst);
    CloseHandle(g_app.wake_event);
    CloseHandle(g_app.stop_event);
    DeleteCriticalSection(&g_app.state_lock);
    CloseHandle(g_app.mutex);
    return 0;
}

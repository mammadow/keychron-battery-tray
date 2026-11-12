#include "hid_win.h"
#include "icon_render.h"
#include "protocol.h"
#include "settings_win.h"
#include "version.h"

#include <dbt.h>
#include <hidsdi.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define WM_TRAY_CALLBACK      (WM_USER + 11)
#define WM_APP_POLL_RESULT    (WM_APP + 1)

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

#define KC_DISPLAY_WAKE_COOLDOWN_MS 60000ULL

/* GUID_CONSOLE_DISPLAY_STATE from winnt.h without requiring <initguid.h>. */
static const GUID KC_GUID_CONSOLE_DISPLAY_STATE = {
    0x6fe69556U, 0x704aU, 0x47a0U, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}
};

typedef struct kc_raw_dev_entry {
    HANDLE hdevice;
    bool is_match;
} kc_raw_dev_entry_t;

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

static void kc_format_status_line(const kc_tray_app_t *app, wchar_t out_line[128]) {
    const wchar_t *name = kc_device_label(app);
    const wchar_t *link = kc_hid_link_label_w(app->hid_session.active_link);

    if (app->poll_state.has_reading) {
        if (app->poll_state.consecutive_failures == 0U) {
            wchar_t detail[64] = L"";
            size_t pos = 0U;
            if (link[0] != L'\0') {
                int n = _snwprintf(detail + pos, 64U - pos, L"%ls", link);
                if (n > 0) pos += (size_t)n;
            }
            if (app->poll_state.last_charging && pos < 63U) {
                int n = _snwprintf(detail + pos, 64U - pos, L"%lscharging", pos > 0U ? L", " : L"");
                if (n > 0) pos += (size_t)n;
            }
            if (app->has_reading_time && pos < 63U) {
                _snwprintf(detail + pos, 64U - pos, L"%ls%02u:%02u",
                           pos > 0U ? L", " : L"",
                           (unsigned)app->last_reading_time.wHour,
                           (unsigned)app->last_reading_time.wMinute);
            }
            detail[63] = L'\0';
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
                                   char out_glyph[8],
                                   wchar_t out_tip[128],
                                   kc_icon_tone_t *out_tone,
                                   bool *out_low_alert,
                                   bool *out_full_alert) {
    *out_tone = KC_TONE_NORMAL;
    *out_low_alert = false;
    *out_full_alert = false;

    if (app->poll_state.has_reading) {
        bool is_sleeping = (app->poll_state.consecutive_failures > 0U &&
                            app->last_status == KC_HID_TIMEOUT);
        bool err_glyph = !is_sleeping && kc_should_show_error_glyph(&app->poll_state);
        if (err_glyph) {
            strncpy(out_glyph, "!", 8);
        } else {
            snprintf(out_glyph, 8, "%u", (unsigned)app->poll_state.last_pct);
            if (app->poll_state.consecutive_failures > 0U) {
                *out_tone = KC_TONE_STALE;
            } else if (app->poll_state.last_charging) {
                *out_tone = KC_TONE_CHARGING;
            } else if (app->settings.low_alert_pct > 0U &&
                       app->poll_state.last_pct <= app->settings.low_alert_pct) {
                *out_tone = KC_TONE_LOW;
            }
        }

        if (app->poll_state.consecutive_failures == 0U) {
            *out_low_alert = kc_check_low_battery_alert(
                &app->poll_state, app->settings.low_alert_pct);
            *out_full_alert = kc_check_full_battery_alert(
                &app->poll_state, app->settings.notify_full);
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
    bool low_alert = false;
    bool full_alert = false;

    kc_build_display_state(app, glyph, tip, &tone, &low_alert, &full_alert);
    bool want_toast = low_alert || full_alert;

    HICON hicon = kc_icon_cache_get(
        &app->icon_cache, glyph, app->icon_size, app->light_taskbar, tone);
    if (!hicon) {
        if (low_alert) app->poll_state.low_battery_notified = false;
        if (full_alert) app->poll_state.full_battery_notified = false;
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

    if (low_alert) {
        nid.dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;
        wcsncpy(nid.szInfoTitle, L"Keychron Mouse Battery Low", 63);
        _snwprintf(nid.szInfo, 255, L"%ls battery is at %u%%.",
                   kc_device_label(app),
                   (unsigned)app->poll_state.last_pct);
        nid.szInfo[255] = L'\0';
    } else if (full_alert) {
        nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        wcsncpy(nid.szInfoTitle, L"Keychron Mouse Charged", 63);
        _snwprintf(nid.szInfo, 255, L"%ls battery is fully charged (100%%).",
                   kc_device_label(app));
        nid.szInfo[255] = L'\0';
    }

    if (force_add || !app->tray_added) {
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        if (want_toast) {
            nid.uFlags |= NIF_INFO;
        }
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            app->tray_added = true;
        } else {
            if (low_alert) app->poll_state.low_battery_notified = false;
            if (full_alert) app->poll_state.full_battery_notified = false;
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
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    app->last_hicon = hicon;
    app->last_tone = tone;
    strncpy(app->last_glyph, glyph, sizeof(app->last_glyph) - 1);
    app->last_glyph[sizeof(app->last_glyph) - 1] = '\0';
    wcsncpy(app->last_tip, tip, 127);
    app->last_tip[127] = L'\0';
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
        if (epoch_match) {
            if (st == KC_HID_OK) {
                kc_poll_state_record_success(&app->poll_state, pct, charging, GetTickCount64());
            } else {
                kc_poll_state_record_failure(&app->poll_state);
            }
        }
        uint32_t normal_ms = app->settings.poll_minutes * 60U * 1000U;
        uint32_t wait_ms = kc_next_wait_ms_for(&app->poll_state, normal_ms);
        LeaveCriticalSection(&app->state_lock);

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
    LeaveCriticalSection(&app->state_lock);

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

    char json[1280];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"vid\":\"0x%04x\",\"pid\":\"0x%04x\",\"interface\":%d,"
             "\"link\":\"%s\",\"product\":\"%s\"%s,\"has_reading\":%s,\"battery\":%u,"
             "\"charging\":%s,\"status\":\"%s\",\"failures\":%u,\"raw_wake_armed\":%s%s}",
             KC_APP_VERSION, (unsigned)vid, (unsigned)pid, iface,
             link_str, prod_esc, paired_buf,
             has_reading ? "true" : "false", (unsigned)pct,
             charging ? "true" : "false", status_esc, (unsigned)fails,
             raw_armed ? "true" : "false", est_buf);
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

static void kc_show_context_menu(kc_tray_app_t *app) {
    HMENU menu = CreatePopupMenu();
    HMENU dev_sub = CreatePopupMenu();
    HMENU poll_sub = CreatePopupMenu();
    HMENU alert_sub = CreatePopupMenu();
    if (!menu || !dev_sub || !poll_sub || !alert_sub) {
        if (dev_sub) DestroyMenu(dev_sub);
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
    uint32_t poll_minutes = 30U;
    uint8_t low_pct = 15U;
    bool notify_full = true;
    bool has_est = false;

    EnterCriticalSection(&app->state_lock);
    poll_minutes = app->settings.poll_minutes;
    low_pct = app->settings.low_alert_pct;
    notify_full = app->settings.notify_full;
    kc_format_status_line(app, status_line);
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
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)dev_sub, L"Device");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)poll_sub, L"Poll interval");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)alert_sub, L"Alerts");
    AppendMenuW(menu,
                MF_STRING | (kc_autostart_is_enabled() ? MF_CHECKED : MF_UNCHECKED),
                IDM_AUTOSTART,
                L"Run at Windows startup");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_COPY_DIAG, L"Copy diagnostics");
    AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_VERSION, L"Keychron Battery Tray v" KC_WIDE(KC_APP_VERSION));
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(app->hwnd);
    UINT cmd = (UINT)TrackPopupMenuEx(
        menu,
        TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x,
        pt.y,
        app->hwnd,
        NULL);
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
        case IDM_REFRESH:
            kc_request_refresh(app, false);
            break;
        case IDM_COPY_DIAG:
            kc_copy_diagnostics(app);
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
    if (!app || !app->hwnd || app->raw_watch_armed == enable) {
        return;
    }
    RAWINPUTDEVICE rid;
    memset(&rid, 0, sizeof(rid));
    rid.usUsagePage = 0x01U; /* HID_USAGE_PAGE_GENERIC */
    rid.usUsage = 0x02U;     /* HID_USAGE_GENERIC_MOUSE */
    rid.dwFlags = enable ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    rid.hwndTarget = enable ? app->hwnd : NULL;

    if (RegisterRawInputDevices(&rid, 1U, sizeof(rid))) {
        app->raw_watch_armed = enable;
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
            if (lparam == WM_LBUTTONUP) {
                kc_request_refresh(app, false);
            } else if (lparam == WM_RBUTTONUP) {
                kc_show_context_menu(app);
            }
            return 0;

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

#include "hid_win.h"
#include "icon_render.h"
#include "protocol.h"
#include "settings_win.h"
#include "version.h"

#include <dbt.h>
#include <hidsdi.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define WM_TRAY_CALLBACK      (WM_USER + 11)
#define WM_APP_POLL_RESULT    (WM_APP + 1)

#define TRAY_ICON_UID         1U
#define TIMER_RESUME_REFRESH  1U
#define TIMER_USB_REFRESH     2U

#define IDM_STATUS            1001U
#define IDM_REFRESH           1002U
#define IDM_AUTOSTART         1003U
#define IDM_QUIT              1004U
#define IDM_VERSION           1005U

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

#define KC_DISPLAY_WAKE_COOLDOWN_MS 60000ULL

/* GUID_CONSOLE_DISPLAY_STATE from winnt.h without requiring <initguid.h>. */
static const GUID KC_GUID_CONSOLE_DISPLAY_STATE = {
    0x6fe69556U, 0x704aU, 0x47a0U, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}
};

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
    kc_hid_status_t last_status;
    SYSTEMTIME last_reading_time;
    bool has_reading_time;
    wchar_t device_product[128];

    char last_glyph[8];
    wchar_t last_tip[128];
    HICON last_hicon;
    int icon_size;
    bool light_taskbar;
} kc_tray_app_t;

static kc_tray_app_t g_app;

static void kc_parse_tray_args(kc_tray_app_t *app) {
    app->target_vid = KEYCHRON_VID;
    app->target_pid = 0U;
    app->target_interface = -1;

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
            }
        } else if (wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc) {
            uint16_t p = 0U;
            if (kc_parse_u16_w(argv[++i], &p)) {
                app->target_pid = p;
            }
        } else if (wcscmp(argv[i], L"--interface") == 0 && i + 1 < argc) {
            int iface = -1;
            if (kc_parse_int_range_w(argv[++i], 0, 255, &iface)) {
                app->target_interface = iface;
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

static void kc_build_display_state(kc_tray_app_t *app,
                                   char out_glyph[8],
                                   wchar_t out_tip[128],
                                   bool *out_low_alert) {
    *out_low_alert = false;
    const wchar_t *name = kc_device_label(app);

    if (app->poll_state.has_reading) {
        if (kc_should_show_error_glyph(&app->poll_state)) {
            strncpy(out_glyph, "!", 8);
        } else {
            _snprintf(out_glyph, 8, "%u", (unsigned)app->poll_state.last_pct);
        }

        if (app->poll_state.consecutive_failures == 0U) {
            *out_low_alert = kc_check_low_battery_alert(&app->poll_state, app->settings.low_alert_pct);
            if (app->has_reading_time) {
                _snwprintf(out_tip, 128,
                           L"%ls: %u%% (%02u:%02u)",
                           name,
                           (unsigned)app->poll_state.last_pct,
                           (unsigned)app->last_reading_time.wHour,
                           (unsigned)app->last_reading_time.wMinute);
            } else {
                _snwprintf(out_tip, 128,
                           L"%ls: %u%%",
                           name,
                           (unsigned)app->poll_state.last_pct);
            }
        } else {
            _snwprintf(out_tip, 128,
                       L"%ls: %u%% (%ls)",
                       name,
                       (unsigned)app->poll_state.last_pct,
                       kc_hid_status_text_w(app->last_status));
        }
    } else if (app->poll_state.consecutive_failures > 0U) {
        strncpy(out_glyph, "!", 8);
        _snwprintf(out_tip, 128,
                   L"%ls: %ls",
                   name,
                   kc_hid_status_text_w(app->last_status));
    } else {
        strncpy(out_glyph, "--", 8);
        _snwprintf(out_tip, 128, L"%ls: checking...", name);
    }

    out_glyph[7] = '\0';
    out_tip[127] = L'\0';
}

static void kc_sync_tray_icon(kc_tray_app_t *app, bool force_add) {
    char glyph[8];
    wchar_t tip[128];
    bool low_alert = false;

    kc_build_display_state(app, glyph, tip, &low_alert);

    HICON hicon = kc_icon_cache_get(&app->icon_cache, glyph, app->icon_size, app->light_taskbar);
    if (!hicon) {
        return;
    }

    bool icon_changed = (hicon != app->last_hicon) || (strcmp(glyph, app->last_glyph) != 0);
    bool tip_changed = (wcscmp(tip, app->last_tip) != 0);

    if (!force_add && app->tray_added && !icon_changed && !tip_changed && !low_alert) {
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

    if (force_add || !app->tray_added) {
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            app->tray_added = true;
        }
    } else {
        nid.uFlags = NIF_SHOWTIP;
        if (icon_changed) {
            nid.uFlags |= NIF_ICON;
        }
        if (tip_changed) {
            nid.uFlags |= NIF_TIP;
        }
        if (low_alert) {
            nid.uFlags |= NIF_INFO;
            nid.dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;
            wcsncpy(nid.szInfoTitle, L"Keychron Mouse Battery Low", 63);
            _snwprintf(nid.szInfo, 255, L"%ls battery is at %u%%.",
                       kc_device_label(app),
                       (unsigned)app->poll_state.last_pct);
        }
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    app->last_hicon = hicon;
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
    bool already_running = app->refresh_in_flight;
    LeaveCriticalSection(&app->state_lock);

    if (!already_running || reset_cache) {
        SetEvent(app->wake_event);
    }
}

static DWORD WINAPI kc_worker_proc(LPVOID param) {
    kc_tray_app_t *app = (kc_tray_app_t *)param;
    HANDLE waits[2] = {app->stop_event, app->wake_event};

    while (WaitForSingleObject(app->stop_event, 0) != WAIT_OBJECT_0) {
        ResetEvent(app->wake_event);

        EnterCriticalSection(&app->state_lock);
        app->refresh_in_flight = true;
        uint32_t start_epoch = app->session_epoch;
        kc_hid_session_t local_session = app->hid_session;
        uint16_t vid = app->target_vid;
        uint16_t pid = app->target_pid;
        int iface = app->target_interface;
        bool in_backoff = (app->poll_state.consecutive_failures > 0U);
        LeaveCriticalSection(&app->state_lock);

        /* Use 1 fast attempt during sleep backoff, 2 attempts when healthy/interactive. */
        uint32_t retries = in_backoff ? 1U : 2U;
        uint32_t timeout_ms = in_backoff ? 1000U : 1500U;

        uint8_t pct = 0;
        kc_hid_status_t st = kc_hid_query_battery(
            &local_session, vid, pid, iface, retries, timeout_ms,
            app->stop_event, false, NULL, &pct);

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
        if (st == KC_HID_OK) {
            kc_poll_state_record_success(&app->poll_state, pct, GetTickCount64());
        } else {
            kc_poll_state_record_failure(&app->poll_state);
        }
        uint32_t normal_ms = app->settings.poll_minutes * 60U * 1000U;
        uint32_t wait_ms = kc_next_wait_ms_for(&app->poll_state, normal_ms);
        LeaveCriticalSection(&app->state_lock);

        PostMessageW(app->hwnd, WM_APP_POLL_RESULT, (WPARAM)st, (LPARAM)pct);

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

static void kc_show_context_menu(kc_tray_app_t *app) {
    HMENU menu = CreatePopupMenu();
    HMENU poll_sub = CreatePopupMenu();
    HMENU alert_sub = CreatePopupMenu();
    if (!menu || !poll_sub || !alert_sub) {
        if (poll_sub) DestroyMenu(poll_sub);
        if (alert_sub) DestroyMenu(alert_sub);
        if (menu) DestroyMenu(menu);
        return;
    }

    wchar_t status_line[128];
    uint32_t poll_minutes = 30U;
    uint8_t low_pct = 15U;

    EnterCriticalSection(&app->state_lock);
    poll_minutes = app->settings.poll_minutes;
    low_pct = app->settings.low_alert_pct;
    const wchar_t *name = kc_device_label(app);
    if (app->poll_state.has_reading && app->has_reading_time) {
        _snwprintf(status_line, 128,
                   L"%ls: %u%% (%02u:%02u)",
                   name,
                   (unsigned)app->poll_state.last_pct,
                   (unsigned)app->last_reading_time.wHour,
                   (unsigned)app->last_reading_time.wMinute);
    } else if (app->poll_state.has_reading) {
        _snwprintf(status_line, 128, L"%ls: %u%%", name, (unsigned)app->poll_state.last_pct);
    } else {
        _snwprintf(status_line, 128, L"%ls: %ls", name, kc_hid_status_text_w(app->last_status));
    }
    status_line[127] = L'\0';
    LeaveCriticalSection(&app->state_lock);

    AppendMenuW(poll_sub, MF_STRING, IDM_POLL_5, L"Every 5 minutes");
    AppendMenuW(poll_sub, MF_STRING, IDM_POLL_15, L"Every 15 minutes");
    AppendMenuW(poll_sub, MF_STRING, IDM_POLL_30, L"Every 30 minutes");
    AppendMenuW(poll_sub, MF_STRING, IDM_POLL_60, L"Every 60 minutes");
    CheckMenuRadioItem(poll_sub, IDM_POLL_5, IDM_POLL_60, kc_poll_menu_id(poll_minutes), MF_BYCOMMAND);

    AppendMenuW(alert_sub, MF_STRING, IDM_ALERT_OFF, L"Off");
    AppendMenuW(alert_sub, MF_STRING, IDM_ALERT_10, L"Below 10%");
    AppendMenuW(alert_sub, MF_STRING, IDM_ALERT_15, L"Below 15%");
    AppendMenuW(alert_sub, MF_STRING, IDM_ALERT_20, L"Below 20%");
    CheckMenuRadioItem(alert_sub, IDM_ALERT_OFF, IDM_ALERT_20, kc_alert_menu_id(low_pct), MF_BYCOMMAND);

    AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_STATUS, status_line);
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"Refresh now");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)poll_sub, L"Poll interval");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)alert_sub, L"Low-battery alert");
    AppendMenuW(menu,
                MF_STRING | (kc_autostart_is_enabled() ? MF_CHECKED : MF_UNCHECKED),
                IDM_AUTOSTART,
                L"Run at Windows startup");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
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
    DestroyMenu(menu); /* Also destroys attached submenus poll_sub and alert_sub. */

    switch (cmd) {
        case IDM_REFRESH:
            kc_request_refresh(app, false);
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
            LeaveCriticalSection(&app->state_lock);
            break;
        }
        case IDM_QUIT:
            DestroyWindow(app->hwnd);
            break;
        default:
            break;
    }
}

static bool kc_is_hid_devinterface_event(LPARAM lparam) {
    const DEV_BROADCAST_HDR *hdr = (const DEV_BROADCAST_HDR *)lparam;
    return hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE;
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

        case WM_APP_POLL_RESULT:
            EnterCriticalSection(&app->state_lock);
            app->last_status = (kc_hid_status_t)wparam;
            if (app->last_status == KC_HID_OK) {
                GetLocalTime(&app->last_reading_time);
                app->has_reading_time = true;
            }
            kc_sync_tray_icon(app, false);
            LeaveCriticalSection(&app->state_lock);
            return 0;

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
                kc_is_hid_devinterface_event(lparam)) {
                SetTimer(hwnd, TIMER_USB_REFRESH, 500U, NULL);
            }
            return TRUE;

        case WM_TIMER:
            if (wparam == TIMER_RESUME_REFRESH || wparam == TIMER_USB_REFRESH) {
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
    kc_parse_tray_args(&g_app);
    kc_settings_load(&g_app.settings);

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
    wc.lpszClassName = L"KeychronBatteryTrayWindow";

    if (!RegisterClassExW(&wc)) {
        CloseHandle(g_app.wake_event);
        CloseHandle(g_app.stop_event);
        DeleteCriticalSection(&g_app.state_lock);
        CloseHandle(g_app.mutex);
        return 1;
    }

    /* Top-level hidden popup window is required to receive TaskbarCreated broadcasts. */
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

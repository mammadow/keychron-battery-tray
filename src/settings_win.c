#include "settings_win.h"
#include "protocol.h"

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#define KC_SETTINGS_SUBKEY L"Software\\KeychronBatteryTray"
#define KC_RUN_SUBKEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define KC_RUN_VALUE       L"KeychronBatteryTray"

static uint32_t kc_clamp_poll_minutes(uint32_t minutes) {
    switch (minutes) {
        case 5U:
        case 15U:
        case 30U:
        case 60U:
            return minutes;
        default:
            return 30U;
    }
}

static uint8_t kc_clamp_low_alert(uint32_t pct) {
    switch (pct) {
        case 0U:
        case 10U:
        case 15U:
        case 20U:
            return (uint8_t)pct;
        default:
            return (uint8_t)KEYCHRON_LOW_BATTERY_PCT;
    }
}

static bool kc_reg_read_dword(const wchar_t *name, DWORD *out) {
    DWORD val = 0;
    DWORD sz = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, KC_SETTINGS_SUBKEY, name,
                     RRF_RT_REG_DWORD, NULL, &val, &sz) == ERROR_SUCCESS) {
        *out = val;
        return true;
    }
    return false;
}

void kc_settings_load(kc_tray_settings_t *out) {
    if (!out) {
        return;
    }
    out->poll_minutes = 30U;
    out->low_alert_pct = (uint8_t)KEYCHRON_LOW_BATTERY_PCT;

    DWORD val = 0;
    if (kc_reg_read_dword(L"PollMinutes", &val)) {
        out->poll_minutes = kc_clamp_poll_minutes(val);
    }
    if (kc_reg_read_dword(L"LowAlertPct", &val)) {
        out->low_alert_pct = kc_clamp_low_alert(val);
    }
}

void kc_settings_save(const kc_tray_settings_t *s) {
    if (!s) {
        return;
    }
    HKEY key = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, KC_SETTINGS_SUBKEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) {
        return;
    }
    DWORD poll_m = kc_clamp_poll_minutes(s->poll_minutes);
    DWORD low_p = kc_clamp_low_alert(s->low_alert_pct);
    RegSetValueExW(key, L"PollMinutes", 0, REG_DWORD, (const BYTE *)&poll_m, sizeof(poll_m));
    RegSetValueExW(key, L"LowAlertPct", 0, REG_DWORD, (const BYTE *)&low_p, sizeof(low_p));
    RegCloseKey(key);
}

bool kc_autostart_is_enabled(void) {
    wchar_t buf[MAX_PATH + 64];
    DWORD size = sizeof(buf);
    LSTATUS st = RegGetValueW(
        HKEY_CURRENT_USER,
        KC_RUN_SUBKEY,
        KC_RUN_VALUE,
        RRF_RT_REG_SZ,
        NULL,
        buf,
        &size);
    return st == ERROR_SUCCESS && buf[0] != L'\0';
}

void kc_autostart_set(bool enable, uint16_t vid, uint16_t pid, int interface_num) {
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, KC_RUN_SUBKEY, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }

    if (!enable) {
        RegDeleteValueW(key, KC_RUN_VALUE);
        RegCloseKey(key);
        return;
    }

    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    if (len > 0U && len < MAX_PATH) {
        wchar_t cmd[MAX_PATH + 80];
        int pos = _snwprintf(cmd, sizeof(cmd) / sizeof(cmd[0]), L"\"%ls\"", exe_path);
        if (pos > 0 && vid != KEYCHRON_VID) {
            pos += _snwprintf(cmd + pos, (sizeof(cmd) / sizeof(cmd[0])) - (size_t)pos,
                              L" --vid 0x%04x", vid);
        }
        if (pos > 0 && pid != 0U) {
            pos += _snwprintf(cmd + pos, (sizeof(cmd) / sizeof(cmd[0])) - (size_t)pos,
                              L" --pid 0x%04x", pid);
        }
        if (pos > 0 && interface_num >= 0) {
            _snwprintf(cmd + pos, (sizeof(cmd) / sizeof(cmd[0])) - (size_t)pos,
                       L" --interface %d", interface_num);
        }
        cmd[(sizeof(cmd) / sizeof(cmd[0])) - 1] = L'\0';
        DWORD bytes = (DWORD)((wcslen(cmd) + 1U) * sizeof(wchar_t));
        RegSetValueExW(key, KC_RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd, bytes);
    }
    RegCloseKey(key);
}

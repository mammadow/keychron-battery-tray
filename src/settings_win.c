#include "settings_win.h"
#include "protocol.h"

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#define KC_SETTINGS_SUBKEY L"Software\\KeychronBatteryTray"
#define KC_RUN_SUBKEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define KC_RUN_VALUE       L"KeychronBatteryTray"

static bool kc_reg_read_dword_at(const wchar_t *subkey, const wchar_t *name, DWORD *out) {
    DWORD val = 0;
    DWORD sz = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, subkey, name,
                     RRF_RT_REG_DWORD, NULL, &val, &sz) == ERROR_SUCCESS) {
        *out = val;
        return true;
    }
    return false;
}

static bool kc_reg_read_dword(const wchar_t *name, DWORD *out) {
    return kc_reg_read_dword_at(KC_SETTINGS_SUBKEY, name, out);
}

static bool kc_reg_read_qword_at(const wchar_t *subkey, const wchar_t *name, uint64_t *out) {
    uint64_t val = 0ULL;
    DWORD sz = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, subkey, name,
                     RRF_RT_REG_QWORD, NULL, &val, &sz) == ERROR_SUCCESS) {
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
    out->notify_full = true;
    out->target_pid = 0U;
    out->target_interface = -1;

    DWORD val = 0;
    if (kc_reg_read_dword(L"PollMinutes", &val)) {
        out->poll_minutes = kc_clamp_poll_minutes(val);
    }
    if (kc_reg_read_dword(L"LowAlertPct", &val)) {
        out->low_alert_pct = kc_clamp_low_alert(val);
    }
    if (kc_reg_read_dword(L"NotifyFull", &val)) {
        out->notify_full = (val != 0UL);
    }
    if (kc_reg_read_dword(L"TargetPid", &val) && val <= 0xFFFFUL) {
        out->target_pid = (uint16_t)val;
    }
    if (kc_reg_read_dword(L"TargetInterface", &val)) {
        int32_t iface = (int32_t)val;
        out->target_interface = (iface >= 0 && iface <= 255) ? iface : -1;
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
    DWORD nfull = s->notify_full ? 1UL : 0UL;
    DWORD tpid = (DWORD)s->target_pid;
    DWORD tiface = (s->target_interface >= 0 && s->target_interface <= 255)
        ? (DWORD)s->target_interface
        : 0xFFFFFFFFUL;
    RegSetValueExW(key, L"PollMinutes", 0, REG_DWORD, (const BYTE *)&poll_m, sizeof(poll_m));
    RegSetValueExW(key, L"LowAlertPct", 0, REG_DWORD, (const BYTE *)&low_p, sizeof(low_p));
    RegSetValueExW(key, L"NotifyFull", 0, REG_DWORD, (const BYTE *)&nfull, sizeof(nfull));
    RegSetValueExW(key, L"TargetPid", 0, REG_DWORD, (const BYTE *)&tpid, sizeof(tpid));
    RegSetValueExW(key, L"TargetInterface", 0, REG_DWORD, (const BYTE *)&tiface, sizeof(tiface));
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
        size_t cap = sizeof(cmd) / sizeof(cmd[0]);
        int pos = _snwprintf(cmd, cap, L"\"%ls\"", exe_path);
        if (pos > 0 && (size_t)pos < cap && vid != KEYCHRON_VID) {
            int n = _snwprintf(cmd + pos, cap - (size_t)pos, L" --vid 0x%04x", vid);
            if (n > 0) pos += n;
        }
        if (pos > 0 && (size_t)pos < cap && pid != 0U) {
            int n = _snwprintf(cmd + pos, cap - (size_t)pos, L" --pid 0x%04x", pid);
            if (n > 0) pos += n;
        }
        if (pos > 0 && (size_t)pos < cap && interface_num >= 0) {
            _snwprintf(cmd + pos, cap - (size_t)pos, L" --interface %d", interface_num);
        }
        cmd[cap - 1U] = L'\0';
        DWORD bytes = (DWORD)((wcslen(cmd) + 1U) * sizeof(wchar_t));
        RegSetValueExW(key, KC_RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd, bytes);
    }
    RegCloseKey(key);
}

void kc_protector_load_at(const wchar_t *subkey, kc_protector_persist_t *out, uint64_t now_unix) {
    if (!subkey || !out) {
        return;
    }
    kc_protector_persist_init(out, now_unix);

    DWORD val = 0;
    if (kc_reg_read_dword_at(subkey, L"ProtectorMode", &val)) {
        out->mode = kc_clamp_protector_mode(val);
    }
    if (kc_reg_read_dword_at(subkey, L"ProtectorLaptopGuard", &val)) {
        out->laptop_guard = (val != 0UL);
    }
    if (kc_reg_read_dword_at(subkey, L"ProtectorCalibrating", &val)) {
        out->calibrating = (val != 0UL) && (out->mode != (uint8_t)KC_PROT_OFF);
    }
    if (kc_reg_read_dword_at(subkey, L"ProtectorCumChargedPct", &val)) {
        out->cum_charged_pct = (uint32_t)val;
    }
    uint64_t qv = 0ULL;
    if (kc_reg_read_qword_at(subkey, L"ProtectorLastCalUnix", &qv)) {
        out->last_cal_unix = qv;
    }
    if (kc_reg_read_qword_at(subkey, L"ProtectorSnoozeUntil", &qv)) {
        out->cal_snooze_until = qv;
    }
}

void kc_protector_save_at(const wchar_t *subkey, const kc_protector_persist_t *p) {
    if (!subkey || !p) {
        return;
    }
    HKEY key = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) {
        return;
    }
    DWORD mode = kc_clamp_protector_mode(p->mode);
    DWORD lguard = p->laptop_guard ? 1UL : 0UL;
    DWORD cal = (p->calibrating && mode != (uint32_t)KC_PROT_OFF) ? 1UL : 0UL;
    DWORD cum = (DWORD)p->cum_charged_pct;
    uint64_t last_cal = p->last_cal_unix;
    uint64_t snooze = p->cal_snooze_until;

    RegSetValueExW(key, L"ProtectorMode", 0, REG_DWORD, (const BYTE *)&mode, sizeof(mode));
    RegSetValueExW(key, L"ProtectorLaptopGuard", 0, REG_DWORD, (const BYTE *)&lguard, sizeof(lguard));
    RegSetValueExW(key, L"ProtectorCalibrating", 0, REG_DWORD, (const BYTE *)&cal, sizeof(cal));
    RegSetValueExW(key, L"ProtectorCumChargedPct", 0, REG_DWORD, (const BYTE *)&cum, sizeof(cum));
    RegSetValueExW(key, L"ProtectorLastCalUnix", 0, REG_QWORD, (const BYTE *)&last_cal, sizeof(last_cal));
    RegSetValueExW(key, L"ProtectorSnoozeUntil", 0, REG_QWORD, (const BYTE *)&snooze, sizeof(snooze));
    RegCloseKey(key);
}

void kc_protector_load(kc_protector_persist_t *out, uint64_t now_unix) {
    kc_protector_load_at(KC_SETTINGS_SUBKEY, out, now_unix);
}

void kc_protector_save(const kc_protector_persist_t *p) {
    kc_protector_save_at(KC_SETTINGS_SUBKEY, p);
}

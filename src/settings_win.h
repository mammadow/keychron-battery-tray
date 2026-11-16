#ifndef KEYCHRON_SETTINGS_WIN_H
#define KEYCHRON_SETTINGS_WIN_H

#include "protocol.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct kc_tray_settings {
    uint32_t poll_minutes;
    uint8_t low_alert_pct;
    bool notify_full;
    uint16_t target_pid;
    int32_t target_interface;
} kc_tray_settings_t;

void kc_settings_load(kc_tray_settings_t *out);
void kc_settings_save(const kc_tray_settings_t *s);

/** Load/save Battery Protector & calibration counters under HKCU\Software\KeychronBatteryTray. */
void kc_protector_load(kc_protector_persist_t *out, uint64_t now_unix);
void kc_protector_save(const kc_protector_persist_t *p);

/** Testable subkey-parameterized variants of kc_protector_load / kc_protector_save. */
void kc_protector_load_at(const wchar_t *subkey, kc_protector_persist_t *out, uint64_t now_unix);
void kc_protector_save_at(const wchar_t *subkey, const kc_protector_persist_t *p);

bool kc_autostart_is_enabled(void);
void kc_autostart_set(bool enable, uint16_t vid, uint16_t pid, int interface_num);

#endif /* KEYCHRON_SETTINGS_WIN_H */

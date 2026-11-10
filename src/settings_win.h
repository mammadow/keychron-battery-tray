#ifndef KEYCHRON_SETTINGS_WIN_H
#define KEYCHRON_SETTINGS_WIN_H

#include <stdbool.h>
#include <stdint.h>

typedef struct kc_tray_settings {
    uint32_t poll_minutes;
    uint8_t low_alert_pct;
} kc_tray_settings_t;

void kc_settings_load(kc_tray_settings_t *out);
void kc_settings_save(const kc_tray_settings_t *s);

bool kc_autostart_is_enabled(void);
void kc_autostart_set(bool enable, uint16_t vid, uint16_t pid, int interface_num);

#endif /* KEYCHRON_SETTINGS_WIN_H */

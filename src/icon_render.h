#ifndef KEYCHRON_ICON_RENDER_H
#define KEYCHRON_ICON_RENDER_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

#define KC_ICON_CACHE_CAP 16U

typedef enum kc_icon_tone {
    KC_TONE_NORMAL = 0,
    KC_TONE_LOW = 1,
    KC_TONE_CHARGING = 2,
    KC_TONE_STALE = 3
} kc_icon_tone_t;

typedef struct kc_icon_entry {
    char key[8];
    kc_icon_tone_t tone;
    HICON hicon;
} kc_icon_entry_t;

typedef struct kc_icon_cache {
    kc_icon_entry_t entries[KC_ICON_CACHE_CAP];
    size_t count;
    int icon_size;
    bool light_taskbar;
} kc_icon_cache_t;

void kc_icon_cache_init(kc_icon_cache_t *cache);
void kc_icon_cache_clear(kc_icon_cache_t *cache);
void kc_icon_cache_destroy(kc_icon_cache_t *cache);

/** Query the current DPI-aware small tray icon dimension (pixels). */
int kc_query_tray_icon_size(HWND hwnd);

/** Read Windows SystemUsesLightTheme from HKCU Personalize (false = dark taskbar). */
bool kc_is_taskbar_light(void);

/**
 * Return a cached (or newly rendered) HICON for `text` at `(icon_size, light_taskbar, tone)`.
 * The returned HICON is owned by the cache and valid until cache clear/destroy.
 */
HICON kc_icon_cache_get(kc_icon_cache_t *cache,
                        const char *text,
                        int icon_size,
                        bool light_taskbar,
                        kc_icon_tone_t tone);

#endif /* KEYCHRON_ICON_RENDER_H */

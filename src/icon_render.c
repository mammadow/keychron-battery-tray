#include "icon_render.h"
#include "glyph_blit.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void kc_icon_cache_init(kc_icon_cache_t *cache) {
    if (!cache) {
        return;
    }
    memset(cache, 0, sizeof(*cache));
}

void kc_icon_cache_clear(kc_icon_cache_t *cache) {
    if (!cache) {
        return;
    }
    for (size_t i = 0; i < cache->count; ++i) {
        if (cache->entries[i].hicon) {
            DestroyIcon(cache->entries[i].hicon);
            cache->entries[i].hicon = NULL;
        }
    }
    cache->count = 0;
}

void kc_icon_cache_destroy(kc_icon_cache_t *cache) {
    kc_icon_cache_clear(cache);
}

int kc_query_tray_icon_size(HWND hwnd) {
    UINT dpi = 0U;
    if (hwnd) {
        dpi = GetDpiForWindow(hwnd);
    }
    if (dpi == 0U) {
        dpi = GetDpiForSystem();
    }
    int size = (dpi != 0U)
        ? GetSystemMetricsForDpi(SM_CXSMICON, dpi)
        : GetSystemMetrics(SM_CXSMICON);
    return (size > 0) ? size : 16;
}

bool kc_is_taskbar_light(void) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    LSTATUS st = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme",
        RRF_RT_REG_DWORD,
        NULL,
        &value,
        &size);
    if (st != ERROR_SUCCESS) {
        return false;
    }
    return value != 0U;
}

static int CALLBACK kc_font_enum_cb(const LOGFONTW *lf,
                                    const TEXTMETRICW *tm,
                                    DWORD font_type,
                                    LPARAM lparam) {
    (void)lf;
    (void)tm;
    (void)font_type;
    bool *found = (bool *)lparam;
    *found = true;
    return 0;
}

static const wchar_t *kc_select_taskbar_font(HDC dc) {
    static int cached = -1;
    if (cached < 0) {
        LOGFONTW query;
        memset(&query, 0, sizeof(query));
        query.lfCharSet = DEFAULT_CHARSET;
        wcsncpy(query.lfFaceName, L"Segoe UI Variable Text", LF_FACESIZE - 1);
        bool found = false;
        EnumFontFamiliesExW(dc, &query, kc_font_enum_cb, (LPARAM)&found, 0);
        cached = found ? 1 : 0;
    }
    return (cached == 1) ? L"Segoe UI Variable Text" : L"Segoe UI";
}

static HBITMAP kc_create_topdown_dib32(HDC dc, int width, int height, uint32_t **out_bits) {
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    *out_bits = (uint32_t *)bits;
    return bmp;
}

static HICON kc_render_tray_icon(const char *text, int icon_size, bool light_taskbar) {
    if (!text || icon_size <= 0) {
        return NULL;
    }

    const int canvas_w = icon_size * 2;
    const int canvas_h = icon_size * 2;

    wchar_t wtext[16];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, (int)(sizeof(wtext) / sizeof(wtext[0])));
    if (wlen <= 0) {
        return NULL;
    }

    HDC screen_dc = GetDC(NULL);
    if (!screen_dc) {
        return NULL;
    }
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    if (!mem_dc) {
        ReleaseDC(NULL, screen_dc);
        return NULL;
    }

    uint32_t *scratch_bits = NULL;
    HBITMAP scratch_bmp = kc_create_topdown_dib32(mem_dc, canvas_w, canvas_h, &scratch_bits);
    uint32_t *dst_bits = NULL;
    HBITMAP dst_bmp = kc_create_topdown_dib32(mem_dc, icon_size, icon_size, &dst_bits);

    if (!scratch_bmp || !scratch_bits || !dst_bmp || !dst_bits) {
        if (scratch_bmp) DeleteObject(scratch_bmp);
        if (dst_bmp) DeleteObject(dst_bmp);
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        return NULL;
    }

    const wchar_t *face = kc_select_taskbar_font(screen_dc);
    HGDIOBJ old_bmp = SelectObject(mem_dc, scratch_bmp);
    SetBkMode(mem_dc, TRANSPARENT);
    SetTextColor(mem_dc, RGB(255, 255, 255));

    /* Windows 11 taskbar text uses 12 DIPs = 0.75 * SM_CXSMICON in Segoe UI Variable Text Regular.
       Shrink 1px at a time if a 3-digit value ("100") exceeds icon_size. */
    int font_px = (int)lround((double)icon_size * 0.75);
    if (font_px < 8) {
        font_px = 8;
    }

    int min_x = 0, min_y = 0, max_x = -1, max_y = -1;
    bool has_ink = false;
    for (;;) {
        memset(scratch_bits, 0, (size_t)canvas_w * (size_t)canvas_h * sizeof(uint32_t));
        HFONT font = CreateFontW(
            -font_px,
            0, 0, 0,
            FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_SWISS,
            face);
        HFONT owned_font = font;
        if (!font) {
            font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        }

        HGDIOBJ old_font = SelectObject(mem_dc, font);
        RECT rc = {0, 0, canvas_w, canvas_h};
        DrawTextW(mem_dc, wtext, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        SelectObject(mem_dc, old_font);
        if (owned_font) {
            DeleteObject(owned_font);
        }

        has_ink = kc_measure_ink_bbox(
            scratch_bits, canvas_w, canvas_h, &min_x, &min_y, &max_x, &max_y);
        if (!has_ink || (max_x - min_x + 1) <= icon_size || font_px <= 8) {
            break;
        }
        font_px--;
    }

    const uint8_t fg = light_taskbar ? 24U : 255U;
    if (has_ink) {
        int gw = max_x - min_x + 1;
        int gh = max_y - min_y + 1;
        int dst_x0 = (icon_size - gw) / 2;
        int dst_y0 = (icon_size - gh) / 2;
        kc_blit_glyph_premul(
            scratch_bits, canvas_w, min_x, min_y, gw, gh,
            dst_bits, icon_size, dst_x0, dst_y0, fg, fg, fg);
    } else {
        memset(dst_bits, 0, (size_t)icon_size * (size_t)icon_size * sizeof(uint32_t));
    }

    /* Unselect GDI objects before passing dst_bmp to CreateIconIndirect. */
    SelectObject(mem_dc, old_bmp);
    DeleteObject(scratch_bmp);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);

    /* Zeroed 1bpp AND mask tells Windows to honor per-pixel ARGB alpha in dst_bmp. */
    size_t mask_stride = (size_t)(((icon_size + 15) / 16) * 2);
    uint8_t *mask_bits = (uint8_t *)calloc(mask_stride * (size_t)icon_size, 1);
    HBITMAP mask_bmp = mask_bits ? CreateBitmap(icon_size, icon_size, 1, 1, mask_bits) : NULL;
    free(mask_bits);

    HICON hicon = NULL;
    if (mask_bmp) {
        ICONINFO ii;
        memset(&ii, 0, sizeof(ii));
        ii.fIcon = TRUE;
        ii.hbmMask = mask_bmp;
        ii.hbmColor = dst_bmp;
        hicon = CreateIconIndirect(&ii);
        DeleteObject(mask_bmp);
    }

    DeleteObject(dst_bmp);
    return hicon;
}

HICON kc_icon_cache_get(kc_icon_cache_t *cache,
                        const char *text,
                        int icon_size,
                        bool light_taskbar) {
    if (!cache || !text) {
        return NULL;
    }
    if (cache->icon_size != icon_size || cache->light_taskbar != light_taskbar) {
        kc_icon_cache_clear(cache);
        cache->icon_size = icon_size;
        cache->light_taskbar = light_taskbar;
    }

    for (size_t i = 0; i < cache->count; ++i) {
        if (strncmp(cache->entries[i].key, text, sizeof(cache->entries[i].key) - 1) == 0) {
            return cache->entries[i].hicon;
        }
    }

    HICON hicon = kc_render_tray_icon(text, icon_size, light_taskbar);
    if (!hicon) {
        return NULL;
    }

    if (cache->count < KC_ICON_CACHE_CAP) {
        kc_icon_entry_t *slot = &cache->entries[cache->count++];
        strncpy(slot->key, text, sizeof(slot->key) - 1);
        slot->key[sizeof(slot->key) - 1] = '\0';
        slot->hicon = hicon;
    } else {
        /* Evict slot 0 if capacity ever filled (bounded at 104 entries). */
        if (cache->entries[0].hicon) {
            DestroyIcon(cache->entries[0].hicon);
        }
        strncpy(cache->entries[0].key, text, sizeof(cache->entries[0].key) - 1);
        cache->entries[0].key[sizeof(cache->entries[0].key) - 1] = '\0';
        cache->entries[0].hicon = hicon;
    }
    return hicon;
}

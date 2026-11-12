#include "icon_render.h"
#include "glyph_blit.h"
#include "hid_win.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KC_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

typedef enum kc_fg_expect {
    KC_FG_WHITE = 0,
    KC_FG_DARK,
    KC_FG_RED,
    KC_FG_GREEN,
    KC_FG_GRAY
} kc_fg_expect_t;

static void verify_hicon_layout(HICON hicon,
                                int size,
                                kc_fg_expect_t fg,
                                int expected_min_h,
                                int expected_max_h) {
    KC_CHECK(hicon != NULL);

    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    KC_CHECK(GetIconInfo(hicon, &ii) != FALSE);
    KC_CHECK(ii.hbmColor != NULL);

    HDC dc = GetDC(NULL);
    KC_CHECK(dc != NULL);

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t *pixels = (uint32_t *)calloc((size_t)size * (size_t)size, sizeof(uint32_t));
    KC_CHECK(pixels != NULL);

    int rows = GetDIBits(dc, ii.hbmColor, 0, (UINT)size, pixels, &bmi, DIB_RGB_COLORS);
    KC_CHECK(rows == size);

    int lit_pixels = 0;
    int transparent_pixels = 0;
    int min_x = size, min_y = size, max_x = -1, max_y = -1;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t px = pixels[y * size + x];
            uint8_t a = (uint8_t)((px >> 24) & 0xFFU);
            uint8_t r = (uint8_t)((px >> 16) & 0xFFU);
            uint8_t g = (uint8_t)((px >> 8) & 0xFFU);
            if (a == 0U) {
                transparent_pixels++;
            } else {
                lit_pixels++;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                if (fg == KC_FG_DARK) {
                    KC_CHECK(r <= 24U);
                } else if (fg == KC_FG_RED) {
                    KC_CHECK(r >= g);
                } else if (fg == KC_FG_GREEN) {
                    uint8_t b = (uint8_t)(px & 0xFFU);
                    KC_CHECK(g >= r && g >= b);
                } else if (fg == KC_FG_GRAY) {
                    uint8_t b = (uint8_t)(px & 0xFFU);
                    KC_CHECK(r == g && g == b && r <= a);
                } else {
                    KC_CHECK(r <= a);
                }
            }
        }
    }
    KC_CHECK(lit_pixels > 0);
    KC_CHECK(transparent_pixels > 0);

    int gh = max_y - min_y + 1;
    if (expected_min_h > 0) {
        KC_CHECK(gh >= expected_min_h && gh <= expected_max_h);
    }

    free(pixels);
    ReleaseDC(NULL, dc);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
}

int main(void) {
    int size = kc_query_tray_icon_size(NULL);
    KC_CHECK(size >= 16);

    kc_icon_cache_t cache;
    kc_icon_cache_init(&cache);

    HICON h46 = kc_icon_cache_get(&cache, "46", 24, false, KC_TONE_NORMAL);
    verify_hicon_layout(h46, 24, KC_FG_WHITE, 13, 13);

    HICON h46_again = kc_icon_cache_get(&cache, "46", 24, false, KC_TONE_NORMAL);
    KC_CHECK(h46_again == h46);

    /* Low-battery, charging, and stale-sleep icon variants use distinct HICONs. */
    HICON h12_norm = kc_icon_cache_get(&cache, "12", 24, false, KC_TONE_NORMAL);
    HICON h12_low = kc_icon_cache_get(&cache, "12", 24, false, KC_TONE_LOW);
    HICON h12_chg = kc_icon_cache_get(&cache, "12", 24, false, KC_TONE_CHARGING);
    HICON h12_stale = kc_icon_cache_get(&cache, "12", 24, false, KC_TONE_STALE);
    KC_CHECK(h12_low != h12_norm && h12_chg != h12_norm && h12_chg != h12_low);
    KC_CHECK(h12_stale != h12_norm && h12_stale != h12_low && h12_stale != h12_chg);
    verify_hicon_layout(h12_low, 24, KC_FG_RED, 13, 13);
    verify_hicon_layout(h12_chg, 24, KC_FG_GREEN, 13, 13);
    verify_hicon_layout(h12_stale, 24, KC_FG_GRAY, 13, 13);

    HICON h100_24 = kc_icon_cache_get(&cache, "100", 24, false, KC_TONE_NORMAL);
    verify_hicon_layout(h100_24, 24, KC_FG_WHITE, 11, 13);

    HICON h100_16 = kc_icon_cache_get(&cache, "100", 16, false, KC_TONE_NORMAL);
    verify_hicon_layout(h100_16, 16, KC_FG_WHITE, 7, 9);

    HICON hdash = kc_icon_cache_get(&cache, "--", 16, false, KC_TONE_NORMAL);
    verify_hicon_layout(hdash, 16, KC_FG_WHITE, 1, 3);

    HICON herr = kc_icon_cache_get(&cache, "!", 16, false, KC_TONE_NORMAL);
    verify_hicon_layout(herr, 16, KC_FG_WHITE, 7, 9);

    HICON h46_light = kc_icon_cache_get(&cache, "46", 24, true, KC_TONE_NORMAL);
    verify_hicon_layout(h46_light, 24, KC_FG_DARK, 13, 13);
    KC_CHECK(cache.count == 1U);

    HICON h12_light_low = kc_icon_cache_get(&cache, "12", 24, true, KC_TONE_LOW);
    verify_hicon_layout(h12_light_low, 24, KC_FG_RED, 13, 13);

    HICON h46_light_chg = kc_icon_cache_get(&cache, "46", 24, true, KC_TONE_CHARGING);
    verify_hicon_layout(h46_light_chg, 24, KC_FG_GREEN, 13, 13);

    HICON h46_light_stale = kc_icon_cache_get(&cache, "46", 24, true, KC_TONE_STALE);
    verify_hicon_layout(h46_light_stale, 24, KC_FG_GRAY, 13, 13);

    kc_icon_cache_destroy(&cache);
    KC_CHECK(cache.count == 0U);

    printf("All keychron Windows GDI icon tests passed (detected tray size=%d).\n", size);
    return 0;
}

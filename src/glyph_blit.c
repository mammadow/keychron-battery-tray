#include "glyph_blit.h"

#include <string.h>

bool kc_measure_ink_bbox(const uint32_t *src_xrgb,
                         int width,
                         int height,
                         int *out_min_x,
                         int *out_min_y,
                         int *out_max_x,
                         int *out_max_y) {
    if (!src_xrgb || width <= 0 || height <= 0) {
        return false;
    }

    int min_x = width;
    int min_y = height;
    int max_x = -1;
    int max_y = -1;

    for (int y = 0; y < height; ++y) {
        const uint32_t *row = src_xrgb + (size_t)y * (size_t)width;
        for (int x = 0; x < width; ++x) {
            if ((row[x] & 0xFFU) != 0U) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }

    if (max_x < min_x || max_y < min_y) {
        return false;
    }
    if (out_min_x) *out_min_x = min_x;
    if (out_min_y) *out_min_y = min_y;
    if (out_max_x) *out_max_x = max_x;
    if (out_max_y) *out_max_y = max_y;
    return true;
}

void kc_blit_glyph_premul(const uint32_t *src_xrgb,
                          int src_stride,
                          int src_x0,
                          int src_y0,
                          int gw,
                          int gh,
                          uint32_t *dst_bgra,
                          int dst_size,
                          int dst_x0,
                          int dst_y0,
                          uint8_t r,
                          uint8_t g,
                          uint8_t b) {
    if (!src_xrgb || !dst_bgra || dst_size <= 0 || gw <= 0 || gh <= 0) {
        return;
    }

    memset(dst_bgra, 0, (size_t)dst_size * (size_t)dst_size * sizeof(uint32_t));

    for (int gy = 0; gy < gh; ++gy) {
        int dy = dst_y0 + gy;
        if (dy < 0 || dy >= dst_size) {
            continue;
        }
        const uint32_t *srow = src_xrgb + (size_t)(src_y0 + gy) * (size_t)src_stride;
        uint32_t *drow = dst_bgra + (size_t)dy * (size_t)dst_size;

        for (int gx = 0; gx < gw; ++gx) {
            int dx = dst_x0 + gx;
            if (dx < 0 || dx >= dst_size) {
                continue;
            }
            uint32_t alpha = srow[src_x0 + gx] & 0xFFU;
            if (alpha == 0U) {
                continue;
            }
            uint32_t pr = ((uint32_t)r * alpha + 127U) / 255U;
            uint32_t pg = ((uint32_t)g * alpha + 127U) / 255U;
            uint32_t pb = ((uint32_t)b * alpha + 127U) / 255U;
            drow[dx] = (alpha << 24) | (pr << 16) | (pg << 8) | pb;
        }
    }
}

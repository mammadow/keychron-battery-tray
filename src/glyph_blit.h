#ifndef KEYCHRON_GLYPH_BLIT_H
#define KEYCHRON_GLYPH_BLIT_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Find the tight non-zero ink bounding box inside a top-down 32-bit XRGB canvas.
 * Returns false if no pixel has non-zero luminance.
 */
bool kc_measure_ink_bbox(const uint32_t *src_xrgb,
                         int width,
                         int height,
                         int *out_min_x,
                         int *out_min_y,
                         int *out_max_x,
                         int *out_max_y);

/**
 * Copy a measured glyph ink sub-rectangle from a grayscale-on-black 32-bit XRGB
 * scratch buffer into a premultiplied BGRA icon buffer at (dst_x0, dst_y0).
 */
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
                          uint8_t b);

#endif /* KEYCHRON_GLYPH_BLIT_H */

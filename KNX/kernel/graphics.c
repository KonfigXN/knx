#include "knx.h"

static volatile uint32_t *pixels;
static uint32_t width, height, pitch, format, masks[4];

static uint32_t chan_lut[3][256];
static uint32_t gray_lut[256];

static void build_luts(void) {
    unsigned i, v;
    if (format <= 1) {
        for (v = 0; v < 256; ++v) {
            if (format == 0) {
                chan_lut[0][v] = (uint32_t)v;
                chan_lut[1][v] = (uint32_t)v << 8;
                chan_lut[2][v] = (uint32_t)v << 16;
            } else {
                chan_lut[0][v] = (uint32_t)v << 16;
                chan_lut[1][v] = (uint32_t)v << 8;
                chan_lut[2][v] = (uint32_t)v;
            }
        }
    } else {
        unsigned shift[3], bits[3];
        uint32_t msk[3];
        for (i = 0; i < 3; ++i) {
            uint32_t m = masks[i];
            shift[i] = 0;
            bits[i] = 0;
            msk[i] = m;
            if (!m)
                continue;
            while (shift[i] < 32 && ((m >> shift[i]) & 1u) == 0)
                ++shift[i];
            while (shift[i] + bits[i] < 32 &&
                   ((m >> (shift[i] + bits[i])) & 1u))
                ++bits[i];
        }
        for (v = 0; v < 256; ++v) {
            for (i = 0; i < 3; ++i) {
                uint32_t c = 0;
                if (msk[i] && bits[i]) {
                    uint32_t scale = bits[i] >= 32 ? 0xFFFFFFFFu
                                                   : ((1u << bits[i]) - 1u);
                    uint32_t q = ((uint32_t)v * scale) / 255u;
                    c = (q << shift[i]) & msk[i];
                }
                chan_lut[i][v] = c;
            }
        }
    }
    for (v = 0; v < 256; ++v)
        gray_lut[v] = chan_lut[0][v] | chan_lut[1][v] | chan_lut[2][v];
}

void knx_early_fb_stamp(const knx_boot_info *info) {
    if (!info || !info->framebuffer_base || !info->framebuffer_width ||
        !info->framebuffer_height || !info->pixels_per_scanline)
        return;
    uint64_t size = (uint64_t)info->pixels_per_scanline * info->framebuffer_height * 4;
    if (!size || info->framebuffer_base > UINT64_MAX - size)
        return;
    if (info->framebuffer_base >= (64ULL << 30) ||
        info->framebuffer_base + size > (64ULL << 30))
        if (knx_vm_map_mmio(info->framebuffer_base, size))
            return;
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)info->framebuffer_base;
    uint32_t pitch = info->pixels_per_scanline;
    for (uint32_t y = 10; y < 110 && y < info->framebuffer_height; ++y)
        for (uint32_t x = 10; x < 110 && x < info->framebuffer_width; ++x)
            fb[y * pitch + x] = 0x0000ff00u;
}

int knx_graphics_init(const knx_boot_info *info) {
    if (!info || !info->framebuffer_base || !info->framebuffer_size ||
        info->framebuffer_width < 8 || info->framebuffer_height < 16 ||
        info->pixels_per_scanline < info->framebuffer_width ||
        info->framebuffer_width > 8192 || info->framebuffer_height > 8192 ||
        info->framebuffer_base > UINT64_MAX - info->framebuffer_size)
        return 0;
    if ((uint64_t)info->pixels_per_scanline * info->framebuffer_height * 4 >
        info->framebuffer_size)
        return 0;
    if (info->pixel_format > 2)
        return 0;

    if (info->framebuffer_base >= (64ULL << 30) ||
        info->framebuffer_base + info->framebuffer_size > (64ULL << 30))
        if (knx_vm_map_mmio(info->framebuffer_base, info->framebuffer_size))
            return 0;
    pixels = (volatile uint32_t *)(uintptr_t)info->framebuffer_base;
    width = info->framebuffer_width;
    height = info->framebuffer_height;
    pitch = info->pixels_per_scanline;
    format = info->pixel_format;
    for (unsigned i = 0; i < 4; ++i)
        masks[i] = info->pixel_masks[i];
    build_luts();
    return 1;
}

static inline uint32_t encode(uint32_t c) {
    return chan_lut[0][(c >> 16) & 255u] |
           chan_lut[1][(c >> 8) & 255u] |
           chan_lut[2][c & 255u];
}

uint32_t knx_gfx_encode_color(uint32_t color) { return encode(color); }

void knx_gfx_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (pixels && x < width && y < height)
        pixels[(uint64_t)y * pitch + x] = encode(color);
}

void knx_gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t color) {
    if (!pixels || !w || !h || x >= width || y >= height)
        return;
    if (w > width - x) w = width - x;
    if (h > height - y) h = height - y;
    uint32_t c = encode(color);
    uint64_t pair = (uint64_t)c | ((uint64_t)c << 32);
    for (uint32_t j = 0; j < h; ++j) {
        volatile uint32_t *row = pixels + (uint64_t)(y + j) * pitch + x;
        if ((((uintptr_t)row) & 7u) == 0) {

            volatile uint64_t *rw = (volatile uint64_t *)row;
            uint32_t n = w >> 1;
            for (uint32_t k = 0; k < n; ++k)
                rw[k] = pair;
            if (w & 1u)
                row[w - 1] = c;
        } else {
            uint32_t i = 0;
            for (; i + 8 <= w; i += 8) {
                row[i    ] = c; row[i + 1] = c;
                row[i + 2] = c; row[i + 3] = c;
                row[i + 4] = c; row[i + 5] = c;
                row[i + 6] = c; row[i + 7] = c;
            }
            for (; i < w; ++i)
                row[i] = c;
        }
    }
}

void knx_gfx_hline(uint32_t x, uint32_t y, uint32_t n, uint32_t c) {
    knx_gfx_fill_rect(x, y, n, 1, c);
}
void knx_gfx_vline(uint32_t x, uint32_t y, uint32_t n, uint32_t c) {
    knx_gfx_fill_rect(x, y, 1, n, c);
}
void knx_gfx_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     uint32_t c) {
    if (!w || !h) return;
    knx_gfx_hline(x, y, w, c); knx_gfx_hline(x, y + h - 1, w, c);
    knx_gfx_vline(x, y, h, c); knx_gfx_vline(x + w - 1, y, h, c);
}

static void row_copy(volatile uint32_t *dst, const volatile uint32_t *src,
                     uint32_t n) {
    if ((uintptr_t)dst > (uintptr_t)src) {
        while (n--)
            dst[n] = src[n];
    } else if ((uintptr_t)dst < (uintptr_t)src) {
        uint32_t i = 0;
        for (; i + 8 <= n; i += 8) {
            dst[i    ] = src[i    ]; dst[i + 1] = src[i + 1];
            dst[i + 2] = src[i + 2]; dst[i + 3] = src[i + 3];
            dst[i + 4] = src[i + 4]; dst[i + 5] = src[i + 5];
            dst[i + 6] = src[i + 6]; dst[i + 7] = src[i + 7];
        }
        for (; i < n; ++i)
            dst[i] = src[i];
    }
}

void knx_gfx_copy(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
                  uint32_t w, uint32_t h) {
    if (!pixels || !w || !h || sx >= width || dx >= width ||
        sy >= height || dy >= height)
        return;
    if (w > width - sx) w = width - sx;
    if (w > width - dx) w = width - dx;
    if (h > height - sy) h = height - sy;
    if (h > height - dy) h = height - dy;
    if (dy > sy) {
        for (uint32_t j = h; j > 0; --j) {
            uint32_t yy = j - 1;
            row_copy(pixels + (uint64_t)(dy + yy) * pitch + dx,
                     pixels + (uint64_t)(sy + yy) * pitch + sx, w);
        }
    } else {
        for (uint32_t j = 0; j < h; ++j)
            row_copy(pixels + (uint64_t)(dy + j) * pitch + dx,
                     pixels + (uint64_t)(sy + j) * pitch + sx, w);
    }
}

void knx_gfx_blit(uint32_t x, uint32_t y, const uint32_t *src,
                  uint32_t w, uint32_t h, uint32_t src_stride) {
    if (!pixels || !src || !w || !h || x >= width || y >= height ||
        src_stride < w)
        return;
    if (w > width - x) w = width - x;
    if (h > height - y) h = height - y;
    for (uint32_t j = 0; j < h; ++j) {
        volatile uint32_t *dst = pixels + (uint64_t)(y + j) * pitch + x;
        const uint32_t *s = src + (size_t)j * src_stride;
        uint32_t i = 0;
        for (; i + 8 <= w; i += 8) {
            dst[i    ] = s[i    ]; dst[i + 1] = s[i + 1];
            dst[i + 2] = s[i + 2]; dst[i + 3] = s[i + 3];
            dst[i + 4] = s[i + 4]; dst[i + 5] = s[i + 5];
            dst[i + 6] = s[i + 6]; dst[i + 7] = s[i + 7];
        }
        for (; i < w; ++i)
            dst[i] = s[i];
    }
}

void knx_gfx_blit_gray(uint32_t x, uint32_t y, const uint8_t *src,
                       uint32_t src_stride, uint32_t w, uint32_t h) {
    if (!pixels || !src || !w || !h || x >= width || y >= height ||
        src_stride < w)
        return;
    if (w > width - x) w = width - x;
    if (h > height - y) h = height - y;
    for (uint32_t j = 0; j < h; ++j) {
        volatile uint32_t *dst = pixels + (uint64_t)(y + j) * pitch + x;
        const uint8_t *s = src + (size_t)j * src_stride;
        for (uint32_t i = 0; i < w; ++i)
            dst[i] = gray_lut[s[i]];
    }
}

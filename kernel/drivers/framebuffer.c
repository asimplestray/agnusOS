#include <framebuffer.h>
#include <multiboot2.h>
#include <stddef.h>

// The global framebuffer state
static framebuffer_t fb;
static int fb_ready = 0;

int fb_init(uint64_t mbi_addr) {
    // Multiboot2 info starts with a fixed 8-byte header (total_size, reserved)
    // then a sequence of variable-length tags. We walk them to find tag type 8.
    struct multiboot_tag* tag = (struct multiboot_tag*)(mbi_addr + 8);

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_FRAMEBUFFER) {
            struct multiboot_tag_framebuffer* fbtag =
                (struct multiboot_tag_framebuffer*)tag;

            // Only handle direct RGB framebuffer (type 1)
            if (fbtag->framebuffer_type != 1) return 0;

            fb.addr          = (uint32_t*)(uintptr_t)fbtag->framebuffer_addr;
            fb.width         = fbtag->framebuffer_width;
            fb.height        = fbtag->framebuffer_height;
            fb.pitch         = fbtag->framebuffer_pitch;
            fb.bpp           = fbtag->framebuffer_bpp;
            fb.pixels_per_row = fbtag->framebuffer_pitch / (fbtag->framebuffer_bpp / 8);
            fb_ready = 1;
            return 1;
        }
        // Tags are 8-byte aligned; advance to next tag
        tag = (struct multiboot_tag*)((uint8_t*)tag + ((tag->size + 7) & ~7));
    }
    return 0; // No framebuffer tag found
}

int fb_is_ready(void) {
    return fb_ready;
}

framebuffer_t* fb_get_info(void) {
    return fb_ready ? &fb : NULL;
}

uint32_t fb_get_width(void)  { return fb.width; }
uint32_t fb_get_height(void) { return fb.height; }

// ── Inline fast pixel put ───────────────────────────────────────────────────

static inline void _put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    fb.addr[y * fb.pixels_per_row + x] = color;
}

// ── Public drawing API ──────────────────────────────────────────────────────

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_ready || x >= fb.width || y >= fb.height) return;
    _put_pixel(x, y, color);
}

uint32_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (!fb_ready || x >= fb.width || y >= fb.height) return 0;
    return fb.addr[y * fb.pixels_per_row + x];
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_ready) return;
    if (x >= fb.width || y >= fb.height) return;
    if (x + w > fb.width)  w = fb.width  - x;
    if (y + h > fb.height) h = fb.height - y;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t* line = fb.addr + (y + row) * fb.pixels_per_row + x;
        for (uint32_t col = 0; col < w; col++) {
            line[col] = color;
        }
    }
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    fb_draw_hline(x,         y,         w, color);
    fb_draw_hline(x,         y + h - 1, w, color);
    fb_draw_vline(x,         y,         h, color);
    fb_draw_vline(x + w - 1, y,         h, color);
}

void fb_draw_hline(uint32_t x, uint32_t y, uint32_t len, uint32_t color) {
    if (!fb_ready || y >= fb.height || x >= fb.width) return;
    if (x + len > fb.width) len = fb.width - x;
    uint32_t* line = fb.addr + y * fb.pixels_per_row + x;
    for (uint32_t i = 0; i < len; i++) line[i] = color;
}

void fb_draw_vline(uint32_t x, uint32_t y, uint32_t len, uint32_t color) {
    if (!fb_ready || x >= fb.width || y >= fb.height) return;
    if (y + len > fb.height) len = fb.height - y;
    for (uint32_t i = 0; i < len; i++) _put_pixel(x, y + i, color);
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, fb.width, fb.height, color);
}

void fb_scroll_up(uint32_t lines, uint32_t bg_color) {
    if (!fb_ready || lines == 0 || lines >= fb.height) return;

    // Move everything `lines` rows upward
    uint32_t copy_rows = fb.height - lines;
    for (uint32_t row = 0; row < copy_rows; row++) {
        uint32_t* dst = fb.addr + row         * fb.pixels_per_row;
        uint32_t* src = fb.addr + (row+lines) * fb.pixels_per_row;
        for (uint32_t col = 0; col < fb.width; col++) {
            dst[col] = src[col];
        }
    }

    // Clear the newly vacated rows at the bottom
    fb_fill_rect(0, copy_rows, fb.width, lines, bg_color);
}

void fb_blit_row(uint32_t dst_x, uint32_t dst_y,
                 const uint32_t* src, uint32_t len) {
    if (!fb_ready) return;
    uint32_t* dst = fb.addr + dst_y * fb.pixels_per_row + dst_x;
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
}

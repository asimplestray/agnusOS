#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>

// --- Color helpers (32-bit ARGB/XRGB format) ---
#define FB_RGB(r, g, b)     ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))
#define FB_RGBA(r, g, b, a) ((uint32_t)(((a) << 24) | ((r) << 16) | ((g) << 8) | (b)))

// Apollo color palette (dark theme, KDE-like)
#define FB_COLOR_BLACK        FB_RGB(0x0D, 0x0E, 0x11)
#define FB_COLOR_DARK_GREY    FB_RGB(0x1A, 0x1B, 0x26)
#define FB_COLOR_GREY         FB_RGB(0x2A, 0x2D, 0x3E)
#define FB_COLOR_LIGHT_GREY   FB_RGB(0x6E, 0x73, 0x8D)
#define FB_COLOR_WHITE        FB_RGB(0xC0, 0xCA, 0xF5)
#define FB_COLOR_ACCENT       FB_RGB(0x7A, 0xA2, 0xF7)   // Soft blue (KDE-like)
#define FB_COLOR_GREEN        FB_RGB(0x9E, 0xCE, 0x6A)
#define FB_COLOR_RED          FB_RGB(0xF7, 0x76, 0x8E)
#define FB_COLOR_YELLOW       FB_RGB(0xE0, 0xAF, 0x68)
#define FB_COLOR_CYAN         FB_RGB(0x7D, 0xCF, 0xFF)
#define FB_COLOR_MAGENTA      FB_RGB(0xBB, 0x9A, 0xF7)
#define FB_COLOR_ORANGE       FB_RGB(0xFF, 0x9E, 0x64)

// Framebuffer state
typedef struct {
    uint32_t* addr;    // Linear framebuffer address (mapped)
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;   // Bytes per row
    uint8_t   bpp;     // Bits per pixel
    uint32_t  pixels_per_row; // pitch / (bpp/8)
} framebuffer_t;

// Initialize framebuffer from multiboot2 info
// Returns 1 on success, 0 if no framebuffer tag found
int  fb_init(uint64_t mbi_addr);

// Query framebuffer dimensions
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);

// --- Drawing Primitives ---

// Put a single pixel at (x, y) with color
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);

// Get a single pixel at (x, y)
uint32_t fb_get_pixel(uint32_t x, uint32_t y);

// Fill a rectangle with a flat color
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

// Draw a rectangle outline
void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

// Draw a horizontal line (fast memset-like path)
void fb_draw_hline(uint32_t x, uint32_t y, uint32_t len, uint32_t color);

// Draw a vertical line
void fb_draw_vline(uint32_t x, uint32_t y, uint32_t len, uint32_t color);

// Clear entire screen to a color
void fb_clear(uint32_t color);

// Scroll the framebuffer up by `lines` pixel rows (used by terminal)
void fb_scroll_up(uint32_t lines, uint32_t bg_color);

// Copy a horizontal strip of pixels (used for font rendering)
void fb_blit_row(uint32_t dst_x, uint32_t dst_y,
                 const uint32_t* src, uint32_t len);

#endif

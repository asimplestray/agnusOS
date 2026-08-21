#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>

// Initialize the graphical terminal.
// Must be called AFTER fb_init() succeeds.
void terminal_init(void);

// Write a single character (handles \n, \r, \b, \t, ANSI escapes)
void terminal_putc(char c);

// Write a null-terminated string
void terminal_print(const char* s);

// Write a hex number (for boot diagnostics)
void terminal_print_hex(uint64_t val);

// Write a decimal number
void terminal_print_dec(uint64_t val);

// Set foreground / background colors (32-bit RGB from FB_COLOR_* palette)
void terminal_set_colors(uint32_t fg, uint32_t bg);

// Clear the terminal screen
void terminal_clear(void);

// Draw / erase the cursor (called by timer for blinking)
void terminal_blink_cursor(void);

// Feed a keystroke into the terminal (called from keyboard IRQ)
void terminal_handle_key(char c);

// Log a boot message with a colored tag: [ TAG ] message
void terminal_log(const char* tag, uint32_t tag_color, const char* message);

// Render the terminal client area inside the Window Manager window
void terminal_render_window(int x, int y, int w, int h);

#endif

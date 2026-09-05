#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>
#include <stddef.h>

// VGA Text Mode Colors
typedef enum {
    COLOR_BLACK = 0,
    COLOR_BLUE = 1,
    COLOR_GREEN = 2,
    COLOR_CYAN = 3,
    COLOR_RED = 4,
    COLOR_MAGENTA = 5,
    COLOR_BROWN = 6,
    COLOR_LIGHT_GREY = 7,
    COLOR_DARK_GREY = 8,
    COLOR_LIGHT_BLUE = 9,
    COLOR_LIGHT_GREEN = 10,
    COLOR_LIGHT_CYAN = 11,
    COLOR_LIGHT_RED = 12,
    COLOR_LIGHT_MAGENTA = 13,
    COLOR_LIGHT_BROWN = 14,
    COLOR_WHITE = 15
} vga_color_t;

// Initialize the screen driver
void screen_init(void);

// Clear the screen with a specific background color
void screen_clear(vga_color_t bg);

// Set the current text color
void screen_set_color(vga_color_t fg, vga_color_t bg);

// Print a character to the screen
void screen_putc(char c);

// Print a string to the screen
void screen_print(const char* str);

// Print a formatted log line with a color tag (e.g. [ OK ] or [ INFO ])
void screen_log(const char* tag, vga_color_t tag_color, const char* message);

// Set the cursor position
void screen_set_cursor(size_t row, size_t col);

// Get current screen dimensions in characters
size_t screen_get_rows(void);
size_t screen_get_cols(void);

// Update screen dimensions based on active display mode
void screen_update_dimensions(void);

#endif

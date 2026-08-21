#include <screen.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_ADDRESS 0xB8000

static uint16_t* const vga_buffer = (uint16_t*)VGA_ADDRESS;
static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;

static inline uint8_t vga_entry_color(vga_color_t fg, vga_color_t bg) {
    return fg | (bg << 4);
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t)uc | ((uint16_t)color << 8);
}

void screen_init(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(COLOR_LIGHT_GREY, COLOR_BLACK);
    screen_clear(COLOR_BLACK);
}

void screen_clear(vga_color_t bg) {
    uint8_t color = vga_entry_color(COLOR_LIGHT_GREY, bg);
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            vga_buffer[index] = vga_entry(' ', color);
        }
    }
    terminal_row = 0;
    terminal_column = 0;
}

void screen_set_color(vga_color_t fg, vga_color_t bg) {
    terminal_color = vga_entry_color(fg, bg);
}

static void screen_scroll(void) {
    // Copy all rows up by one row
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t src_index = (y + 1) * VGA_WIDTH + x;
            const size_t dst_index = y * VGA_WIDTH + x;
            vga_buffer[dst_index] = vga_buffer[src_index];
        }
    }

    // Clear the last row
    const size_t last_row_start = (VGA_HEIGHT - 1) * VGA_WIDTH;
    uint16_t empty_char = vga_entry(' ', vga_entry_color(COLOR_LIGHT_GREY, COLOR_BLACK));
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        vga_buffer[last_row_start + x] = empty_char;
    }

    terminal_row = VGA_HEIGHT - 1;
}

static uint8_t utf8_state = 0;
static uint8_t utf8_lead = 0;

void screen_putc(char c) {
    uint8_t u = (uint8_t)c;

    // Handle UTF-8 multi-byte sequences on-the-fly
    if (utf8_state == 0) {
        if (u == 0xC3 || u == 0xC2) {
            utf8_state = 1;
            utf8_lead = u;
            return;
        }
    } else if (utf8_state == 1) {
        utf8_state = 0;
        if (utf8_lead == 0xC3) {
            switch (u) {
                case 0x87: u = 128; break; // Ç
                case 0xA7: u = 135; break; // ç
                case 0xA0: u = 133; break; // à
                case 0xA1: u = 160; break; // á
                case 0xA2: u = 131; break; // â
                case 0xA3: u = 198; break; // ã
                case 0x84: u = 142; break; // Ä
                case 0x85: u = 143; break; // Å
                case 0x89: u = 144; break; // É
                case 0xA8: u = 138; break; // è
                case 0xA9: u = 130; break; // é
                case 0xAA: u = 136; break; // ê
                case 0xAD: u = 161; break; // í
                case 0xB2: u = 149; break; // ò
                case 0xB3: u = 162; break; // ó
                case 0xB4: u = 147; break; // ô
                case 0xB5: u = 228; break; // õ
                case 0xBA: u = 163; break; // ú
                case 0xBB: u = 150; break; // û
                case 0xBC: u = 129; break; // ü
                default: return; // Ignore unknown/unsupported sequences
            }
        } else if (utf8_lead == 0xC2) {
            switch (u) {
                case 0xAA: u = 166; break; // ª
                case 0xBA: u = 167; break; // º
                case 0xBF: u = 168; break; // ¿
                case 0xA2: u = 155; break; // ¢
                case 0xA3: u = 156; break; // £
                default: return; // Ignore
            }
        }
    }

    if (u == '\n') {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) {
            screen_scroll();
        }
        return;
    }

    if (u == '\t') {
        // Tab stops every 4 columns
        terminal_column = (terminal_column + 4) & ~3;
        if (terminal_column >= VGA_WIDTH) {
            terminal_column = 0;
            if (++terminal_row == VGA_HEIGHT) {
                screen_scroll();
            }
        }
        return;
    }

    const size_t index = terminal_row * VGA_WIDTH + terminal_column;
    vga_buffer[index] = vga_entry(u, terminal_color);

    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) {
            screen_scroll();
        }
    }
}

void screen_print(const char* str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        screen_putc(str[i]);
    }
}

void screen_log(const char* tag, vga_color_t tag_color, const char* message) {
    uint8_t old_color = terminal_color;

    // Print [
    screen_set_color(COLOR_LIGHT_GREY, COLOR_BLACK);
    screen_print("[ ");

    // Print Tag (e.g. OK, INFO, KERNEL)
    screen_set_color(tag_color, COLOR_BLACK);
    screen_print(tag);

    // Print ] 
    screen_set_color(COLOR_LIGHT_GREY, COLOR_BLACK);
    screen_print(" ] ");

    // Print message
    screen_set_color(COLOR_WHITE, COLOR_BLACK);
    screen_print(message);
    screen_print("\n");

    // Restore old color
    terminal_color = old_color;
}

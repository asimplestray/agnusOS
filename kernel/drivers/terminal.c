#include <terminal.h>
#include <framebuffer.h>
#include <font.h>
#include <screen.h>
#include <stddef.h>

// ── Layout ───────────────────────────────────────────────────────────────────

#define MAX_COLS 240
#define MAX_ROWS  80

// Margin from screen edges (pixels)
#define MARGIN_X 12
#define MARGIN_Y 10

// ── Cell storage ─────────────────────────────────────────────────────────────

typedef struct {
    uint8_t  ch;
    uint32_t fg;
    uint32_t bg;
} term_cell_t;

static term_cell_t cells[MAX_ROWS][MAX_COLS];

// ── Terminal state ────────────────────────────────────────────────────────────

static uint32_t term_cols;     // character columns that fit on screen
static uint32_t term_rows;     // character rows
static uint32_t cur_col;       // current cursor column
static uint32_t cur_row;       // current cursor row
static uint32_t cur_fg;        // current foreground color
static uint32_t cur_bg;        // current background color
static int      cursor_visible; // cursor drawn on screen right now?

// UTF-8 decoding state
static uint8_t  utf8_state = 0;
static uint8_t  utf8_lead = 0;

// Terminal window geometry inside WM
static int term_win_x = 202; // Initial client area x
static int term_win_y = 128; // Initial client area y
static int term_win_w = 656; // Initial client area width
static int term_win_h = 410; // Initial client area height

// Default colors — dark Tokyo-Night palette
#define DEFAULT_FG  FB_COLOR_WHITE
#define DEFAULT_BG  FB_COLOR_BLACK

// ── ANSI escape-sequence state machine ───────────────────────────────────────

#define ESC_NONE   0
#define ESC_START  1   // saw ESC
#define ESC_CSI    2   // saw ESC [
#define ESC_ARGS   3   // collecting numeric args

#define MAX_ARGS 8
static int esc_state;
static int esc_args[MAX_ARGS];
static int esc_argc;

// ── ANSI 16-colour palette (maps SGR 30-37 / 90-97) ─────────────────────────
static const uint32_t ansi_colors[16] = {
    FB_COLOR_BLACK,        // 0  black
    FB_COLOR_RED,          // 1  red
    FB_COLOR_GREEN,        // 2  green
    FB_COLOR_YELLOW,       // 3  yellow
    FB_COLOR_ACCENT,       // 4  blue  (use our accent blue)
    FB_COLOR_MAGENTA,      // 5  magenta
    FB_COLOR_CYAN,         // 6  cyan
    FB_COLOR_WHITE,        // 7  white
    FB_COLOR_GREY,         // 8  bright black (dark grey)
    FB_RGB(0xFF,0x87,0x87),// 9  bright red
    FB_RGB(0xB9,0xF2,0x7C),// 10 bright green
    FB_RGB(0xFF,0xE5,0x85),// 11 bright yellow
    FB_RGB(0xA8,0xC7,0xFF),// 12 bright blue
    FB_RGB(0xD9,0xA8,0xFF),// 13 bright magenta
    FB_RGB(0xA4,0xE4,0xFF),// 14 bright cyan
    FB_RGB(0xFF,0xFF,0xFF),// 15 bright white
};

// ── Forward declarations ──────────────────────────────────────────────────────
static void render_cell(uint32_t col, uint32_t row);
static void render_cursor(int show);
static void scroll_up(uint32_t n);
static void advance_cursor(void);

// ── Render a single character cell to the framebuffer ────────────────────────

static void render_cell(uint32_t col, uint32_t row) {
    uint32_t px = term_win_x + MARGIN_X + col * FONT_W;
    uint32_t py = term_win_y + MARGIN_Y + row * FONT_H;
    uint8_t  ch = cells[row][col].ch;
    uint32_t fg = cells[row][col].fg;
    uint32_t bg = cells[row][col].bg;

    // Clamp to safe characters (use space for unprintable)
    if (ch < 0x20) ch = 0x20;

    const uint8_t* glyph = font_8x16[ch];

    for (uint32_t gy = 0; gy < FONT_H; gy++) {
        uint8_t row_bits = glyph[gy];
        for (uint32_t gx = 0; gx < FONT_W; gx++) {
            // bit 7 = leftmost pixel
            uint32_t color = (row_bits & (0x80 >> gx)) ? fg : bg;
            fb_put_pixel(px + gx, py + gy, color);
        }
    }
}

// ── Cursor ────────────────────────────────────────────────────────────────────

static void render_cursor(int show) {
    if (show) {
        // Draw a solid underline bar as cursor
        uint32_t px = term_win_x + MARGIN_X + cur_col * FONT_W;
        uint32_t py = term_win_y + MARGIN_Y + cur_row * FONT_H + FONT_H - 2;
        fb_fill_rect(px, py, FONT_W, 2, cur_fg);
    } else {
        // Erase cursor by re-rendering the cell underneath
        render_cell(cur_col, cur_row);
    }
    cursor_visible = show;
}

void terminal_blink_cursor(void) {
    render_cursor(!cursor_visible);
}

// ── Scrolling ─────────────────────────────────────────────────────────────────

static void scroll_up(uint32_t n) {
    if (n == 0) return;
    if (n >= term_rows) {
        // Clear everything
        for (uint32_t r = 0; r < term_rows; r++)
            for (uint32_t c = 0; c < term_cols; c++)
                cells[r][c] = (term_cell_t){ ' ', DEFAULT_FG, DEFAULT_BG };
    } else {
        // Shift cell buffer up
        for (uint32_t r = 0; r < term_rows - n; r++)
            for (uint32_t c = 0; c < term_cols; c++)
                cells[r][c] = cells[r + n][c];
        // Clear vacated rows at bottom
        for (uint32_t r = term_rows - n; r < term_rows; r++)
            for (uint32_t c = 0; c < term_cols; c++)
                cells[r][c] = (term_cell_t){ ' ', DEFAULT_FG, DEFAULT_BG };
    }

    // Redraw all window terminal cells relative to the current position
    terminal_render_window(term_win_x, term_win_y, term_win_w, term_win_h);
}

// ── Cursor advance & wrap ─────────────────────────────────────────────────────

static void advance_cursor(void) {
    cur_col++;
    if (cur_col >= term_cols) {
        cur_col = 0;
        cur_row++;
        if (cur_row >= term_rows) {
            scroll_up(1);
            cur_row = term_rows - 1;
        }
    }
}

// ── ANSI SGR (color/style) handler ───────────────────────────────────────────

static void handle_sgr(void) {
    if (esc_argc == 0) { esc_args[0] = 0; esc_argc = 1; }
    for (int i = 0; i < esc_argc; i++) {
        int a = esc_args[i];
        if (a == 0) { cur_fg = DEFAULT_FG; cur_bg = DEFAULT_BG; }
        else if (a >= 30 && a <= 37) cur_fg = ansi_colors[a - 30];
        else if (a >= 40 && a <= 47) cur_bg = ansi_colors[a - 40];
        else if (a >= 90 && a <= 97) cur_fg = ansi_colors[a - 90 + 8];
        else if (a >= 100 && a <= 107) cur_bg = ansi_colors[a - 100 + 8];
    }
}

// ── ANSI escape dispatcher ────────────────────────────────────────────────────

static void dispatch_escape(char c) {
    switch (c) {
        case 'm': // SGR
            handle_sgr();
            break;
        case 'H': // Cursor Home
        case 'f':
            cur_row = (esc_argc > 0 && esc_args[0] > 0) ? (uint32_t)(esc_args[0] - 1) : 0;
            cur_col = (esc_argc > 1 && esc_args[1] > 0) ? (uint32_t)(esc_args[1] - 1) : 0;
            if (cur_row >= term_rows) cur_row = term_rows - 1;
            if (cur_col >= term_cols) cur_col = term_cols - 1;
            break;
        case 'J': // Erase display
            terminal_clear();
            break;
        case 'K': // Erase line
            for (uint32_t c2 = cur_col; c2 < term_cols; c2++) {
                cells[cur_row][c2] = (term_cell_t){ ' ', cur_fg, cur_bg };
                render_cell(c2, cur_row);
            }
            break;
        case 'A': { // Cursor up
            uint32_t n = (esc_argc > 0 && esc_args[0] > 0) ? (uint32_t)esc_args[0] : 1;
            cur_row = (cur_row >= n) ? cur_row - n : 0;
            break;
        }
        case 'B': { // Cursor down
            uint32_t n = (esc_argc > 0 && esc_args[0] > 0) ? (uint32_t)esc_args[0] : 1;
            cur_row += n;
            if (cur_row >= term_rows) cur_row = term_rows - 1;
            break;
        }
        case 'C': { // Cursor right
            uint32_t n = (esc_argc > 0 && esc_args[0] > 0) ? (uint32_t)esc_args[0] : 1;
            cur_col += n;
            if (cur_col >= term_cols) cur_col = term_cols - 1;
            break;
        }
        case 'D': { // Cursor left
            uint32_t n = (esc_argc > 0 && esc_args[0] > 0) ? (uint32_t)esc_args[0] : 1;
            cur_col = (cur_col >= n) ? cur_col - n : 0;
            break;
        }
        default:
            break; // Unknown escape — silently ignore
    }
    esc_state = ESC_NONE;
}

// ── Core putc ─────────────────────────────────────────────────────────────────

void terminal_putc(char c) {
    uint8_t u = (uint8_t)c;

    // TEMPORARY: Disable UTF-8 decoding to isolate the issue
    // Handle UTF-8 multi-byte sequences on-the-fly
    /*
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
    */

    // --- ANSI state machine ---
    if (esc_state == ESC_START) {
        if (u == '[') { esc_state = ESC_CSI; esc_argc = 0; esc_args[0] = 0; return; }
        esc_state = ESC_NONE;
        return;
    }
    if (esc_state == ESC_CSI || esc_state == ESC_ARGS) {
        if (u >= '0' && u <= '9') {
            esc_state = ESC_ARGS;
            if (esc_argc == 0) esc_argc = 1;
            esc_args[esc_argc - 1] = esc_args[esc_argc - 1] * 10 + (u - '0');
            return;
        }
        if (u == ';') {
            esc_argc++;
            if (esc_argc < MAX_ARGS) esc_args[esc_argc - 1] = 0;
            return;
        }
        dispatch_escape((char)u);
        return;
    }
    if (u == '\033') { // ESC
        render_cursor(0); // erase cursor before we move
        esc_state = ESC_START;
        return;
    }

    // --- Control characters ---
    // DISABLED: render_cursor(0); - may be causing IRQ deadlock

    switch (u) {
        case '\n':
            cur_col = 0;
            cur_row++;
            if (cur_row >= term_rows) {
                scroll_up(1);
                cur_row = term_rows - 1;
            }
            break;
        case '\r':
            cur_col = 0;
            break;
        case '\b':
            if (cur_col > 0) {
                cur_col--;
                cells[cur_row][cur_col] = (term_cell_t){ ' ', cur_fg, cur_bg };
                render_cell(cur_col, cur_row);
            }
            break;
        case '\t': {
            uint32_t next = (cur_col + 8) & ~7u;
            if (next >= term_cols) next = term_cols - 1;
            while (cur_col < next) {
                cells[cur_row][cur_col] = (term_cell_t){ ' ', cur_fg, cur_bg };
                render_cell(cur_col, cur_row);
                cur_col++;
            }
            break;
        }
        default:
            if (u >= 0x20) {
                cells[cur_row][cur_col] = (term_cell_t){ u, cur_fg, cur_bg };
                render_cell(cur_col, cur_row);
                advance_cursor();
            }
            break;
    }

    // DISABLED: render_cursor(1); - may be causing IRQ deadlock
}

// ── String helpers ────────────────────────────────────────────────────────────

void terminal_print(const char* s) {
    for (; *s; s++) terminal_putc(*s);
}

void terminal_print_hex(uint64_t val) {
    char hex_chars[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[17 - i] = hex_chars[(val >> (i * 4)) & 0xF];
    buf[18] = '\0';
    terminal_print(buf);
}

void terminal_print_dec(uint64_t val) {
    if (val == 0) { terminal_putc('0'); return; }
    char buf[21]; int i = 19; buf[20] = '\0';
    while (val > 0) { buf[i--] = '0' + (val % 10); val /= 10; }
    terminal_print(&buf[i + 1]);
}

// ── Log helper (colored tag, like [ OK ] message) ────────────────────────────

void terminal_log(const char* tag, uint32_t tag_color, const char* message) {
    uint32_t saved_fg = cur_fg;
    uint32_t saved_bg = cur_bg;

    cur_fg = FB_COLOR_LIGHT_GREY; terminal_print("[ ");
    cur_fg = tag_color;           terminal_print(tag);
    cur_fg = FB_COLOR_LIGHT_GREY; terminal_print(" ] ");
    cur_fg = FB_COLOR_WHITE;      terminal_print(message);
    terminal_putc('\n');

    cur_fg = saved_fg;
    cur_bg = saved_bg;
}

// ── Public API ────────────────────────────────────────────────────────────────

void terminal_set_colors(uint32_t fg, uint32_t bg) {
    cur_fg = fg;
    cur_bg = bg;
}

void terminal_clear(void) {
    for (uint32_t r = 0; r < term_rows; r++)
        for (uint32_t c = 0; c < term_cols; c++) {
            cells[r][c] = (term_cell_t){ ' ', cur_fg, cur_bg };
        }
    fb_fill_rect(term_win_x, term_win_y, term_win_w, term_win_h, cur_bg);
    cur_row = 0;
    cur_col = 0;
    cursor_visible = 0;
}

void terminal_init(void) {
    term_cols = (term_win_w - 2 * MARGIN_X) / FONT_W;
    term_rows = (term_win_h - 2 * MARGIN_Y) / FONT_H;
    if (term_cols > MAX_COLS) term_cols = MAX_COLS;
    if (term_rows > MAX_ROWS) term_rows = MAX_ROWS;

    cur_col = 0;
    cur_row = 0;
    cur_fg = DEFAULT_FG;
    cur_bg = DEFAULT_BG;
    cursor_visible = 0;
    esc_state = ESC_NONE;

    // Clear cell buffer
    for (uint32_t r = 0; r < term_rows; r++)
        for (uint32_t c = 0; c < term_cols; c++)
            cells[r][c] = (term_cell_t){ ' ', DEFAULT_FG, DEFAULT_BG };
}

#include <wm.h>

// ── Keyboard input handler ────────────────────────────────────────────────────

void terminal_handle_key(char c) {
    // Only echo input when terminal window is focused
    window_t* focused = wm_get_focused_window();

    if (focused && focused->type == WINDOW_TYPE_TERMINAL) {
        terminal_putc(c);
    }
}

// ── Render callback for the window manager ────────────────────────────────────

void terminal_render_window(int x, int y, int w, int h) {
    term_win_x = x;
    term_win_y = y;
    term_win_w = w;
    term_win_h = h;

    term_cols = (w - 2 * MARGIN_X) / FONT_W;
    term_rows = (h - 2 * MARGIN_Y) / FONT_H;
    if (term_cols > MAX_COLS) term_cols = MAX_COLS;
    if (term_rows > MAX_ROWS) term_rows = MAX_ROWS;

    // Draw the background black client area
    fb_fill_rect(x, y, w, h, cur_bg);

    // Draw all cells
    for (uint32_t r = 0; r < term_rows; r++) {
        for (uint32_t c = 0; c < term_cols; c++) {
            uint32_t px = x + MARGIN_X + c * FONT_W;
            uint32_t py = y + MARGIN_Y + r * FONT_H;
            uint8_t ch = cells[r][c].ch;
            if (ch < 0x20) ch = 0x20;
            const uint8_t* glyph = font_8x16[ch];
            for (uint32_t gy = 0; gy < FONT_H; gy++) {
                uint8_t row_bits = glyph[gy];
                for (uint32_t gx = 0; gx < FONT_W; gx++) {
                    uint32_t color = (row_bits & (0x80 >> gx)) ? cells[r][c].fg : cells[r][c].bg;
                    fb_put_pixel(px + gx, py + gy, color);
                }
            }
        }
    }

    // Draw cursor
    if (cursor_visible) {
        uint32_t px = x + MARGIN_X + cur_col * FONT_W;
        uint32_t py = y + MARGIN_Y + cur_row * FONT_H + FONT_H - 2;
        fb_fill_rect(px, py, FONT_W, 2, cur_fg);
    }
}

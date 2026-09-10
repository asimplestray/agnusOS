#include <wm.h>
#include <framebuffer.h>
#include <font.h>
#include <mouse.h>
#include <screen.h>
#include <stddef.h>

// Stacking order array (index 0 is back, index num-1 is front)
static window_t* windows[WM_MAX_WINDOWS];
static int num_windows = 0;

static uint8_t prev_buttons = 0;

// ── String & character rendering helpers for decorations ─────────────────────

static void wm_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg, int transparent) {
    if ((uint8_t)c < 0x20) return;
    const uint8_t* glyph = font_8x16[(uint8_t)c];
    for (int gy = 0; gy < 16; gy++) {
        uint8_t row = glyph[gy];
        for (int gx = 0; gx < 8; gx++) {
            if (row & (0x80 >> gx)) {
                fb_put_pixel(x + gx, y + gy, fg);
            } else if (!transparent) {
                fb_put_pixel(x + gx, y + gy, bg);
            }
        }
    }
}

static void wm_draw_string(int x, int y, const char* s, uint32_t fg, uint32_t bg, int transparent) {
    for (int i = 0; s[i] != '\0'; i++) {
        wm_draw_char(x + i * 8, y, s[i], fg, bg, transparent);
    }
}

// ── Window Manager Core API ──────────────────────────────────────────────────

void wm_init(void) {
    num_windows = 0;
    prev_buttons = 0;
    
    // Draw initial desktop environment
    wm_draw_desktop();
}

void wm_add_window(window_t* win) {
    if (num_windows >= WM_MAX_WINDOWS) return;
    
    windows[num_windows++] = win;
    
    // Set focus to the newly added window
    for (int i = 0; i < num_windows - 1; i++) {
        windows[i]->is_focused = 0;
    }
    win->is_focused = 1;
    win->is_closed = 0;
    win->is_dragging = 0;
    // Ensure type is set if not already
    if (win->type == 0) win->type = WINDOW_TYPE_OTHER;
    
    // Redraw screen with new window
    mouse_hide();
    wm_draw_desktop();
    mouse_draw();
}

void wm_draw_desktop(void) {
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    // 1. Draw desktop background wallpaper (Tokyo Night dark)
    fb_clear(FB_COLOR_BLACK);
    
    // Subtle background dots or panel accents
    for (uint32_t y = 64; y < screen_h; y += 64) {
        for (uint32_t x = 64; x < screen_w; x += 64) {
            fb_put_pixel(x, y, FB_COLOR_GREY);
        }
    }

    // 2. Draw Top Menu Bar / Panel
    fb_fill_rect(0, 0, screen_w, 32, FB_COLOR_DARK_GREY);
    // KDE-like slim blue border under panel
    fb_fill_rect(0, 31, screen_w, 1, FB_COLOR_ACCENT);

    // Left aligned OS name and branding
    wm_draw_string(12, 8, "AgnusOS", FB_COLOR_ACCENT, 0, 1);
    wm_draw_string(88, 8, "|  System Desktop Shell (v0.2-Alpha)", FB_COLOR_WHITE, 0, 1);
    
    // Find active window for panel status
    window_t* active_win = wm_get_focused_window();
    if (active_win && !active_win->is_closed) {
        wm_draw_string(screen_w - 240, 8, "Active:", FB_COLOR_LIGHT_GREY, 0, 1);
        wm_draw_string(screen_w - 180, 8, active_win->title, FB_COLOR_ACCENT, 0, 1);
    } else {
        wm_draw_string(screen_w - 240, 8, "Workspace Idle", FB_COLOR_LIGHT_GREY, 0, 1);
    }

    // 3. Composite windows from back to front (index 0 to num_windows - 1)
    for (int i = 0; i < num_windows; i++) {
        window_t* win = windows[i];
        if (win->is_closed) continue;

        // Colors based on focus
        uint32_t header_color = win->is_focused ? FB_COLOR_ACCENT : FB_COLOR_GREY;
        uint32_t border_color = win->is_focused ? FB_COLOR_ACCENT : FB_COLOR_GREY;
        uint32_t text_color   = win->is_focused ? FB_COLOR_DARK_GREY : FB_COLOR_WHITE;

        // Window background
        fb_fill_rect(win->x, win->y, win->w, win->h, FB_COLOR_DARK_GREY);

        // Window title bar header
        fb_fill_rect(win->x, win->y, win->w, 28, header_color);

        // Window title text
        wm_draw_string(win->x + 8, win->y + 6, win->title, text_color, 0, 1);

        // Window close button [ X ]
        // A neat box at the top right of the window header
        int close_btn_x = win->x + win->w - 24;
        int close_btn_y = win->y + 6;
        fb_fill_rect(close_btn_x, close_btn_y, 16, 16, FB_COLOR_RED);
        wm_draw_string(close_btn_x + 4, close_btn_y, "x", FB_COLOR_WHITE, 0, 1);

        // Window outer borders (Left, Right, Bottom)
        // Header acts as top border
        fb_fill_rect(win->x, win->y + 28, 2, win->h - 28, border_color); // Left
        fb_fill_rect(win->x + win->w - 2, win->y + 28, 2, win->h - 28, border_color); // Right
        fb_fill_rect(win->x, win->y + win->h - 2, win->w, 2, border_color); // Bottom

        // 4. Render window inner client contents (using its callback)
        if (win->render_content) {
            // Client area resides inside the borders and below the header
            win->render_content(win->x + 2, win->y + 28, win->w - 4, win->h - 30);
        }
    }
}

int wm_update(void) {
    int mx, my;
    uint8_t buttons;
    mouse_get_state(&mx, &my, &buttons);

    // Transition flags
    int clicked  = (buttons & MOUSE_BTN_LEFT) && !(prev_buttons & MOUSE_BTN_LEFT);
    int released = !(buttons & MOUSE_BTN_LEFT) && (prev_buttons & MOUSE_BTN_LEFT);

    int redraw_needed = 0;

    if (clicked) {
        // Walk windows from front to back (top of stack to bottom)
        for (int i = num_windows - 1; i >= 0; i--) {
            window_t* win = windows[i];
            if (win->is_closed) continue;

            // Check if click occurred within window boundaries
            if (mx >= win->x && mx < win->x + win->w &&
                my >= win->y && my < win->y + win->h) {

                // Focus clicked window: move it to the top of the stack (end of array)
                if (i != num_windows - 1) {
                    for (int j = i; j < num_windows - 1; j++) {
                        windows[j] = windows[j + 1];
                    }
                    windows[num_windows - 1] = win;
                }

                // Set focus flag for front window, disable for others
                for (int j = 0; j < num_windows - 1; j++) {
                    windows[j]->is_focused = 0;
                }
                win->is_focused = 1;

                // Case A: Clicked close button
                if (mx >= win->x + win->w - 24 && mx < win->x + win->w - 8 &&
                    my >= win->y + 6 && my < win->y + 22) {
                    win->is_closed = 1;
                    redraw_needed = 1;
                    break;
                }

                // Case B: Clicked window header title bar (start dragging)
                if (my >= win->y && my < win->y + 28) {
                    win->is_dragging = 1;
                    win->drag_offset_x = mx - win->x;
                    win->drag_offset_y = my - win->y;
                }

                redraw_needed = 1;
                break;
            }
        }
    }

    if (released) {
        // Release dragging state for all windows
        for (int i = 0; i < num_windows; i++) {
            windows[i]->is_dragging = 0;
        }
    }

    if (buttons & MOUSE_BTN_LEFT) {
        // Handle active dragging window
        for (int i = 0; i < num_windows; i++) {
            window_t* win = windows[i];
            if (win->is_dragging && !win->is_closed) {
                int new_x = mx - win->drag_offset_x;
                int new_y = my - win->drag_offset_y;

                // Prevent window from getting completely lost off screen edges
                if (new_x < -win->w + 40)   new_x = -win->w + 40;
                if (new_x > 1280 - 40)      new_x = 1280 - 40;
                if (new_y < 32)             new_y = 32; // Stay below top panel
                if (new_y > 800 - 40)       new_y = 800 - 40;

                if (new_x != win->x || new_y != win->y) {
                    win->x = new_x;
                    win->y = new_y;
                    redraw_needed = 1;
                }
            }
        }
    }

    prev_buttons = buttons;

    if (redraw_needed) {
        mouse_hide();
        wm_draw_desktop();
        mouse_draw();
        return 1;
    }

    return 0;
}

window_t* wm_get_focused_window(void) {
    if (num_windows == 0) return NULL;
    window_t* top = windows[num_windows - 1];
    if (top && !top->is_closed && top->is_focused) {
        return top;
    }
    return NULL;
}

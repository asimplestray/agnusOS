#ifndef WM_H
#define WM_H

#include <stdint.h>

#define WM_MAX_WINDOWS 8

// Window type identifiers
typedef enum {
    WINDOW_TYPE_TERMINAL = 1,
    WINDOW_TYPE_OTHER = 0
} window_type_t;

// Window structure
typedef struct {
    int x, y;
    int w, h;
    const char* title;
    int is_focused;
    int is_dragging;
    int drag_offset_x;
    int drag_offset_y;
    int is_closed;
    window_type_t type;
    // Callback to draw the window client area (x, y are absolute screen coordinates of the client area top-left)
    void (*render_content)(int x, int y, int w, int h);
} window_t;

// Initialize Window Manager
void wm_init(void);

// Add a window to the desktop manager
void wm_add_window(window_t* win);

// Composites the entire desktop (wallpaper, panel, window frames, window contents)
// Note: This does NOT draw the mouse cursor, since mouse cursor is drawn on top
void wm_draw_desktop(void);

// Updates window dragging/focus logic based on current mouse state
// Returns 1 if a full desktop redraw was done, 0 otherwise
int wm_update(void);

// Get the currently focused window
window_t* wm_get_focused_window(void);

#endif

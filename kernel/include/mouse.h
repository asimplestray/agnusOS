#ifndef MOUSE_H_INCLUDED
#define MOUSE_H_INCLUDED

#include <stdint.h>

// Pointer Dimensions
#define MOUSE_W 12
#define MOUSE_H 19

// Mouse buttons mask
#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

// Initialize PS/2 Mouse driver
void mouse_init(void);

// Fetch current mouse cursor position and button state mask
void mouse_get_state(int* out_x, int* out_y, uint8_t* out_buttons);

// Helper to check if cursor was updated.
// Resets the update flag so we don't redraw unnecessarily.
int mouse_has_moved(void);

// Restore background and redraw the cursor at the current position.
// This is used by the window manager when drawing the desktop.
void mouse_draw(void);

// Hide the cursor by restoring the pixels underneath it
void mouse_hide(void);

#endif

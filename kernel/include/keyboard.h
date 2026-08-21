#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <wait.h>

#define LAYOUT_US 0
#define LAYOUT_ABNT2 1

// Initialize the PS/2 keyboard driver on IRQ1
void keyboard_init(void);

// Set the active keyboard layout (LAYOUT_US or LAYOUT_ABNT2)
void keyboard_set_layout(int layout);

/* Keyboard buffer operations for STDIN */
extern wait_queue_head_t kbd_wait;
char keyboard_pop_char(void);

#endif

#include <keyboard.h>
#include <idt.h>
#include <io.h>
#include <screen.h>
#include <spinlock.h>
#include <tty.h>

#define KBD_BUF_SIZE 256
static char kbd_buf[KBD_BUF_SIZE];
static int kbd_head = 0;
static int kbd_tail = 0;
static spinlock_irq_t kbd_lock = { SPINLOCK_INIT, 0 };
wait_queue_head_t kbd_wait;

void keyboard_push_char(char c) {
    unsigned long flags;
    spin_lock_irqsave(&kbd_lock, &flags);
    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) {
        kbd_buf[kbd_head] = c;
        kbd_head = next;
    }
    spin_unlock_irqrestore(&kbd_lock, flags);
    wake_up(&kbd_wait);
}

char keyboard_pop_char(void) {
    unsigned long flags;
    spin_lock_irqsave(&kbd_lock, &flags);
    if (kbd_head == kbd_tail) {
        spin_unlock_irqrestore(&kbd_lock, flags);
        return 0;
    }
    char c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    spin_unlock_irqrestore(&kbd_lock, flags);
    return c;
}

// Modifier states
static int shift_active = 0;
static int capslock_active = 0;

// Active layout: Defaults to ABNT2 for our user, but easily toggled!
static int active_layout = LAYOUT_ABNT2;

// --- US Keyboard Layout Maps ---
static const char us_map[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',	/* 0-9 */
  '9', '0', '-', '=', '\b',	/* Backspace */
  '\t',			/* Tab */
  'q', 'w', 'e', 'r',	/* 16-19 */
  't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',	/* Enter key */
    0,			/* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',	/* 30-39 */
 '\'', '`',   0,		/* 40-42 - Left shift */
 '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.',	/* 43-52 */
  '/',   0,			/* 53-54 - Right shift */
  '*',
    0,	/* Alt */
  ' ',	/* Space bar */
    0,	/* Caps lock */
};

static const char us_map_shift[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*',	/* 0-9 */
  '(', ')', '_', '+', '\b',	/* Backspace */
  '\t',			/* Tab */
  'Q', 'W', 'E', 'R',	/* 16-19 */
  'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',	/* Enter key */
    0,			/* 29   - Control */
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',	/* 30-39 */
 '\"', '~',   0,		/* 40-42 - Left shift */
  '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>',	/* 43-52 */
  '?',   0,			/* 53-54 - Right shift */
  '*',
    0,	/* Alt */
  ' ',	/* Space bar */
    0,	/* Caps lock */
};

// --- Brazilian ABNT2 Keyboard Layout Maps ---
static const char abnt2_map[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', /* 0-9 */
  '9', '0', '-', '=', '\b', /* Backspace */
  '\t',         /* Tab */
  'q', 'w', 'e', 'r', /* 16-19 */
  't', 'y', 'u', 'i', 'o', 'p', '\'', '[', '\n', /* Enter key - note 26 is acute accent in ABNT2 */
    0,          /* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0, /* 39 is 'ç' in ABNT2 */
  '~', '`',   0,        /* 40-42 - Left shift */
  ']', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', /* 43-52 - 43 is ']' in ABNT2 */
  ';',   0,         /* 53-54 - 53 is ';' in ABNT2 */
  '*',
    0,  /* Alt */
  ' ',  /* Space bar */
    0,  /* Caps lock */
};

static const char abnt2_map_shift[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', /* 0-9 */
  '(', ')', '_', '+', '\b', /* Backspace */
  '\t',         /* Tab */
  'Q', 'W', 'E', 'R', /* 16-19 */
  'T', 'Y', 'U', 'I', 'O', 'P', '\"', '{', '\n', /* Enter key */
    0,          /* 29   - Control */
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0, /* 39 is 'Ç' in ABNT2 */
  '^', '~',   0,        /* 40-42 - Left shift */
  '}', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', /* 43-52 */
  ':',   0,         /* 53-54 - Right shift */
  '*',
    0,  /* Alt */
  ' ',  /* Space bar */
    0,  /* Caps lock */
};

// PS/2 Keyboard IRQ Callback
static void keyboard_callback(struct interrupt_frame* frame) {
    (void)frame;

    // Read the scancode from keyboard controller data port 0x60
    uint8_t scancode = inb(0x60);

    // Acknowledge the keyboard controller by reading status register
    // This may be needed to clear the interrupt condition
    inb(0x64);

    // If the top bit is set, it means a key was released (Break Code)
    if (scancode & 0x80) {
        uint8_t released_scancode = scancode & ~0x80;

        // Track Left and Right Shift release
        if (released_scancode == 0x2A || released_scancode == 0x36) {
            shift_active = 0;
        }
        return;
    }

    // Capture Left and Right Shift presses
    if (scancode == 0x2A || scancode == 0x36) {
        shift_active = 1;
        return;
    }

    // Capture Caps Lock press (toggle state)
    if (scancode == 0x3A) {
        capslock_active = !capslock_active;
        return;
    }

    // Standard translation variables
    char c = 0;
    int is_uppercase = shift_active ^ capslock_active;

    // --- Special Key Handling for ABNT2 ---
    if (active_layout == LAYOUT_ABNT2) {
        // Special case: 'ç' / 'Ç' (scancode 0x27)
        if (scancode == 0x27) {
            screen_putc(is_uppercase ? 'C' : 'c'); // simplificado
            return;
        }
        // Special case: '/' / '?' key (scancode 0x73 in ABNT2)
        else if (scancode == 0x73) {
            c = shift_active ? '?' : '/';
        }
        // Special case: '\' / '|' key next to Left Shift (scancode 0x56 in ABNT2)
        else if (scancode == 0x56) {
            c = shift_active ? '|' : '\\';
        }
    }

    // Default table-based translation
    if (c == 0) {
        if (scancode < sizeof(us_map)) {
            if (active_layout == LAYOUT_ABNT2) {
                c = shift_active ? abnt2_map_shift[scancode] : abnt2_map[scancode];
            } else {
                c = shift_active ? us_map_shift[scancode] : us_map[scancode];
            }
        }
    }

    // Verify it is a valid, printable character or action
    if (c != 0) {
        // Capitalize alphabet keys if CapsLock/Shift is active
        if (c >= 'a' && c <= 'z') {
            if (is_uppercase) {
                c -= 32; // Convert to uppercase ASCII
            }
        }
        
        if (console_tty) {
            tty_input_char(console_tty, c);
        } else {
            screen_putc(c);
            keyboard_push_char(c);
        }
    }
}

void keyboard_init(void) {
    init_waitqueue_head(&kbd_wait);

    // Register keyboard handler at interrupt 33 (IRQ 1)
    interrupts_register_handler(33, keyboard_callback);

    // Unmask IRQ1 on the master PIC (bit 1 must be 0)
    uint8_t mask = inb(0x21);
    outb(0x21, mask & ~0x02);
}

void keyboard_set_layout(int layout) {
    if (layout == LAYOUT_US || layout == LAYOUT_ABNT2) {
        active_layout = layout;
    }
}

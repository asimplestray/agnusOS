#include <mouse.h>
#include <framebuffer.h>
#include <io.h>
#include <idt.h>

// Global mouse coordinates and state
static int mouse_x = 640;
static int mouse_y = 400;
static int old_mouse_x = 640;
static int old_mouse_y = 400;
static uint8_t mouse_buttons = 0;

static int moved_flag = 0;
static int cursor_visible = 0;

// Save-under buffer
static uint32_t save_under[MOUSE_H * MOUSE_W];

// Premium modern cursor pointer style (+1 para o NUL de cada linha)
static const char mouse_pointer[MOUSE_H][MOUSE_W + 1] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X...XXXXXXX ",
    "X..X.X      ",
    "X.X  X.X    ",
    "XX    X.X   ",
    "X      X.X  ",
    "        X.X ",
    "        X.X ",
    "         XX ",
    "            "
};

// ── software cursor drawing/restoring routines ──────────────────────────────

void mouse_hide(void) {
    if (!cursor_visible) return;
    
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    for (int j = 0; j < MOUSE_H; j++) {
        int py = old_mouse_y + j;
        if (py < 0 || py >= (int)h) continue;
        for (int i = 0; i < MOUSE_W; i++) {
            int px = old_mouse_x + i;
            if (px < 0 || px >= (int)w) continue;

            if (mouse_pointer[j][i] != ' ') {
                fb_put_pixel(px, py, save_under[j * MOUSE_W + i]);
            }
        }
    }
    cursor_visible = 0;
}

void mouse_draw(void) {
    mouse_hide(); // Erase old position first

    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    // 1. Save pixels under the new mouse cursor position
    for (int j = 0; j < MOUSE_H; j++) {
        int py = mouse_y + j;
        if (py < 0 || py >= (int)h) continue;
        for (int i = 0; i < MOUSE_W; i++) {
            int px = mouse_x + i;
            if (px < 0 || px >= (int)w) continue;

            if (mouse_pointer[j][i] != ' ') {
                save_under[j * MOUSE_W + i] = fb_get_pixel(px, py);
            }
        }
    }

    // 2. Draw new cursor pixels on the screen
    for (int j = 0; j < MOUSE_H; j++) {
        int py = mouse_y + j;
        if (py < 0 || py >= (int)h) continue;
        for (int i = 0; i < MOUSE_W; i++) {
            int px = mouse_x + i;
            if (px < 0 || px >= (int)w) continue;

            char pixel_type = mouse_pointer[j][i];
            if (pixel_type == 'X') {
                // Dark border
                fb_put_pixel(px, py, FB_RGB(0x16, 0x16, 0x1E));
            } else if (pixel_type == '.') {
                // White cursor fill
                fb_put_pixel(px, py, FB_RGB(0xC0, 0xCA, 0xF5));
            }
        }
    }

    old_mouse_x = mouse_x;
    old_mouse_y = mouse_y;
    cursor_visible = 1;
}

// ── State accessors ─────────────────────────────────────────────────────────

void mouse_get_state(int* out_x, int* out_y, uint8_t* out_buttons) {
    if (out_x) *out_x = mouse_x;
    if (out_y) *out_y = mouse_y;
    if (out_buttons) *out_buttons = mouse_buttons;
}

int mouse_has_moved(void) {
    int moved = moved_flag;
    moved_flag = 0;
    return moved;
}

// ── PS/2 controller communication and ISR ────────────────────────────────────

static void mouse_write(uint8_t data) {
    // Wait for PS/2 controller input buffer to be empty
    while (inb(0x64) & 2);
    // Write 0xD4 (send command to auxiliary device)
    outb(0x64, 0xD4);
    // Wait again
    while (inb(0x64) & 2);
    // Write the actual command data to 0x60
    outb(0x60, data);
}

static uint8_t mouse_read(void) {
    // Wait for output buffer to be full
    while (!(inb(0x64) & 1));
    return inb(0x60);
}

static uint8_t mouse_cycle = 0;
static uint8_t mouse_bytes[3];

static void mouse_callback(struct interrupt_frame* frame) {
    (void)frame;

    uint8_t status = inb(0x64);
    // Check if output buffer is full AND it's mouse data (auxiliary device bit 5 = 1)
    if (!(status & 1) || !(status & 0x20)) {
        return;
    }

    uint8_t data = inb(0x60);

    // Byte 0 synchronization (bit 3 should be 1)
    if (mouse_cycle == 0 && !(data & 0x08)) {
        return;
    }

    mouse_bytes[mouse_cycle++] = data;

    if (mouse_cycle == 3) {
        mouse_cycle = 0;

        uint8_t flags = mouse_bytes[0];
        // Check for coordinate overflow flags (discard packet if set)
        if (flags & 0x40 || flags & 0x80) {
            return;
        }

        // Relative movement sign extensions (handled automatically by casting to int8_t)
        int delta_x = (int)((int8_t)mouse_bytes[1]);
        int delta_y = (int)((int8_t)mouse_bytes[2]);

        mouse_x += delta_x;
        mouse_y -= delta_y; // Invert Y delta (downwards is positive screen Y)

        // Clamp positions to actual screen coordinates
        uint32_t w = fb_get_width();
        uint32_t h = fb_get_height();
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_x >= (int)w) mouse_x = (int)w - 1;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_y >= (int)h) mouse_y = (int)h - 1;

        // Button clicks
        mouse_buttons = flags & 0x07;

        moved_flag = 1;
    }
}

void mouse_init(void) {
    // 1. Enable auxiliary mouse device
    while (inb(0x64) & 2);
    outb(0x64, 0xA8);

    // 2. Enable mouse interrupts in controller configuration
    while (inb(0x64) & 2);
    outb(0x64, 0x20); // Read command byte
    uint8_t config = mouse_read();

    config |= 0x02;   // Enable IRQ 12
    config &= ~0x20;  // Disable mouse lock clock

    while (inb(0x64) & 2);
    outb(0x64, 0x60); // Write command byte
    while (inb(0x64) & 2);
    outb(0x60, config);

    // 3. Set default mouse configurations
    mouse_write(0xF6); // Set defaults
    mouse_read();      // Read ACK (0xFA)

    // 4. Start streaming coordinate packets
    mouse_write(0xF4); // Enable data reporting
    mouse_read();      // Read ACK (0xFA)

    // 5. Register IRQ 12 handler (interrupt 44)
    interrupts_register_handler(44, mouse_callback);

    // 6. Unmask PIC Interrupts
    // Unmask IRQ2 on Master PIC (Cascade line)
    outb(0x21, inb(0x21) & ~0x04);
    // Unmask IRQ12 on Slave PIC
    outb(0xA1, inb(0xA1) & ~0x10);

    // Initial draw
    mouse_draw();
}

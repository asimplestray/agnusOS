#include <io.h>

#define SERIAL_COM1 0x3F8

void serial_init(void) {
    outb(SERIAL_COM1 + 1, 0x00);    // Disable all interrupts
    outb(SERIAL_COM1 + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(SERIAL_COM1 + 0, 0x01);    // Set divisor to 1 (lo byte) 115200 baud
    outb(SERIAL_COM1 + 1, 0x00);    //                  (hi byte)
    outb(SERIAL_COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(SERIAL_COM1 + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(SERIAL_COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

static inline int serial_is_transmit_empty(void) {
    return inb(SERIAL_COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    while (!serial_is_transmit_empty());
    outb(SERIAL_COM1, c);
}

void serial_print(const char *str) {
    while (*str) {
        serial_putc(*str++);
    }
}
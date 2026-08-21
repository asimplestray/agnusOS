#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>
#include <idt.h>

void log_init(void);
void log_write(const char *str);
void kprintf(const char *fmt, ...);

void panic(const char *file, int line, const char *func, const char *fmt, ...);
void dump_registers(struct interrupt_frame *frame);
int log_get_len(void);
void log_read(char *buf, int len, int *out_len);

#define PANIC(fmt, ...) \
    panic(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            PANIC("Assertion failed: %s", #cond); \
        } \
    } while (0)

#endif
#include <panic.h>
#include <serial.h>
#include <screen.h>
#include <spinlock.h>
#include <task.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#define LOG_BUFFER_SIZE 4096

static char log_buffer[LOG_BUFFER_SIZE];
static uint32_t log_head = 0;
static uint32_t log_tail = 0;
static spinlock_irq_t log_lock = { SPINLOCK_INIT, 0 };
static bool log_initialized = false;

static inline void log_putc_locked(char c) {
    log_buffer[log_head] = c;
    log_head = (log_head + 1) % LOG_BUFFER_SIZE;
    if (log_head == log_tail) {
        log_tail = (log_tail + 1) % LOG_BUFFER_SIZE;
    }
}

void log_init(void) {
    unsigned long flags;
    spin_lock_irqsave(&log_lock, &flags);
    log_head = 0;
    log_tail = 0;
    for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
        log_buffer[i] = 0;
    }
    log_initialized = true;
    spin_unlock_irqrestore(&log_lock, flags);
}

void log_write(const char *str) {
    if (!log_initialized) return;
    
    unsigned long flags;
    spin_lock_irqsave(&log_lock, &flags);
    while (*str) {
        log_putc_locked(*str++);
    }
    spin_unlock_irqrestore(&log_lock, flags);
}

void log_write_hex(uint64_t val) {
    char hex[17];
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = val & 0xF;
        hex[15 - i] = (nibble < 10) ? '0' + nibble : 'A' + (nibble - 10);
        val >>= 4;
    }
    hex[16] = '\0';
    log_write("0x");
    log_write(hex);
}

void log_write_dec(uint64_t val) {
    char buf[21];
    int i = 0;
    if (val == 0) {
        log_write("0");
        return;
    }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (i > 0) {
        log_putc_locked(buf[--i]);
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    char buffer[256];
    int buf_pos = 0;
    
    while (*fmt && buf_pos < 255) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 's': {
                    const char *s = va_arg(args, const char *);
                    while (*s && buf_pos < 255) buffer[buf_pos++] = *s++;
                    break;
                }
                case 'x': {
                    uint64_t val = va_arg(args, uint64_t);
                    char hex[17];
                    for (int i = 15; i >= 0; i--) {
                        uint8_t nibble = val & 0xF;
                        hex[15 - i] = (nibble < 10) ? '0' + nibble : 'A' + (nibble - 10);
                        val >>= 4;
                    }
                    hex[16] = '\0';
                    buffer[buf_pos++] = '0';
                    buffer[buf_pos++] = 'x';
                    for (int i = 0; i < 16 && buf_pos < 255; i++) {
                        buffer[buf_pos++] = hex[i];
                    }
                    break;
                }
                case 'd': {
                    uint64_t val = va_arg(args, uint64_t);
                    char dec[21];
                    int i = 0;
                    if (val == 0) {
                        dec[i++] = '0';
                    } else {
                        while (val > 0) {
                            dec[i++] = '0' + (val % 10);
                            val /= 10;
                        }
                    }
                    while (i > 0 && buf_pos < 255) {
                        buffer[buf_pos++] = dec[--i];
                    }
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    if (buf_pos < 255) buffer[buf_pos++] = c;
                    break;
                }
                default:
                    if (buf_pos < 255) buffer[buf_pos++] = *fmt;
                    break;
            }
        } else {
            if (buf_pos < 255) buffer[buf_pos++] = *fmt;
        }
        fmt++;
    }
    buffer[buf_pos] = '\0';
    
    log_write(buffer);
    serial_print(buffer);
    screen_print(buffer);
    
    va_end(args);
}

void panic(const char *file, int line, const char *func, const char *fmt, ...) {
    __asm__ volatile("cli");
    
    va_list args;
    va_start(args, fmt);
    
    kprintf("\n!!! KERNEL PANIC !!!\n");
    kprintf("Location: %s:%d in %s\n", file, line, func);
    kprintf("Message: ");
    
    char buffer[256];
    int buf_pos = 0;
    
    while (*fmt && buf_pos < 255) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 's': {
                    const char *s = va_arg(args, const char *);
                    while (*s && buf_pos < 255) buffer[buf_pos++] = *s++;
                    break;
                }
                case 'x': {
                    uint64_t val = va_arg(args, uint64_t);
                    char hex[17];
                    for (int i = 15; i >= 0; i--) {
                        uint8_t nibble = val & 0xF;
                        hex[15 - i] = (nibble < 10) ? '0' + nibble : 'A' + (nibble - 10);
                        val >>= 4;
                    }
                    hex[16] = '\0';
                    buffer[buf_pos++] = '0';
                    buffer[buf_pos++] = 'x';
                    for (int i = 0; i < 16 && buf_pos < 255; i++) {
                        buffer[buf_pos++] = hex[i];
                    }
                    break;
                }
                case 'd': {
                    uint64_t val = va_arg(args, uint64_t);
                    char dec[21];
                    int i = 0;
                    if (val == 0) {
                        dec[i++] = '0';
                    } else {
                        while (val > 0) {
                            dec[i++] = '0' + (val % 10);
                            val /= 10;
                        }
                    }
                    while (i > 0 && buf_pos < 255) {
                        buffer[buf_pos++] = dec[--i];
                    }
                    break;
                }
                default:
                    if (buf_pos < 255) buffer[buf_pos++] = *fmt;
                    break;
            }
        } else {
            if (buf_pos < 255) buffer[buf_pos++] = *fmt;
        }
        fmt++;
    }
    buffer[buf_pos] = '\0';
    
    kprintf("%s\n", buffer);
    
    va_end(args);
    
    /* Dump registers from current task */
    if (current) {
        kprintf("\n=== Current Task Registers ===\n");
        kprintf("PID: %d\n", current->pid);
        kprintf("RIP: 0x%lx\n", current->rip);
        kprintf("RSP: 0x%lx\n", current->rsp);
        kprintf("RBP: 0x%lx\n", current->rbp);
        kprintf("RFLAGS: 0x%lx\n", current->rflags);
        kprintf("RAX: 0x%lx  RBX: 0x%lx  RCX: 0x%lx  RDX: 0x%lx\n", 
                current->rax, current->rbx, current->rcx, current->rdx);
        kprintf("RSI: 0x%lx  RDI: 0x%lx  R8:  0x%lx  R9:  0x%lx\n", 
                current->rsi, current->rdi, current->r8, current->r9);
        kprintf("R10: 0x%lx  R11: 0x%lx  R12: 0x%lx  R13: 0x%lx\n", 
                current->r10, current->r11, current->r12, current->r13);
        kprintf("R14: 0x%lx  R15: 0x%lx\n", current->r14, current->r15);
    }
    
    /* Simple backtrace using RBP chain */
    kprintf("\n=== Backtrace ===\n");
    if (current) {
        uint64_t rbp = current->rbp;
        int frame = 0;
        while (rbp && frame < 32) {
            uint64_t *frame_ptr = (uint64_t *)rbp;
            uint64_t ret_addr = frame_ptr[1];  // Return address is at RBP+8
            kprintf("#%d  RBP=0x%lx  RIP=0x%lx\n", frame, rbp, ret_addr);
            rbp = frame_ptr[0];  // Previous RBP is at RBP
            frame++;
        }
    }
    
    /* Dump log buffer */
    kprintf("\n=== Log Buffer (last 512 chars) ===\n");
    unsigned long flags;
    spin_lock_irqsave(&log_lock, &flags);
    uint32_t start = (log_head > 512) ? log_head - 512 : 0;
    for (uint32_t i = 0; i < 512; i++) {
        uint32_t idx = (start + i) % LOG_BUFFER_SIZE;
        char c = log_buffer[idx];
        if (c >= 32 && c <= 126) {
            serial_putc(c);
        } else if (c == '\n' || c == '\r' || c == '\t') {
            serial_putc(c);
        }
    }
    spin_unlock_irqrestore(&log_lock, flags);
    serial_print("\n");

    /* Last chance to save dirty filesystem data before halting */
    extern int ata_sync(void);
    kprintf("\nFlushing disk cache... ");
    if (ata_sync() == 0) kprintf("done.\n");
    else kprintf("skipped (no disk).\n");

    kprintf("\nSystem halted.\n");
    
    while (1) {
        __asm__ volatile("hlt");
    }
}

void dump_registers(struct interrupt_frame *frame) {
    kprintf("\n=== Exception Register Dump ===\n");
    kprintf("INT: %d (err %d)\n", (uint64_t)frame->int_no, (uint64_t)frame->err_code);
    kprintf("RIP: 0x%lx  RSP: 0x%lx  RFLAGS: 0x%lx\n",
            frame->rip, frame->rsp, frame->rflags);
    kprintf("RAX: 0x%lx  RBX: 0x%lx  RCX: 0x%lx  RDX: 0x%lx\n",
            (uint64_t)frame->rax, (uint64_t)frame->rbx,
            (uint64_t)frame->rcx, (uint64_t)frame->rdx);
    kprintf("RBP: 0x%lx  RSI: 0x%lx  RDI: 0x%lx\n",
            (uint64_t)frame->rbp, (uint64_t)frame->rsi, (uint64_t)frame->rdi);
}

int log_get_len(void) {
    unsigned long flags;
    spin_lock_irqsave(&log_lock, &flags);
    int len = (log_head >= log_tail) ? (log_head - log_tail) : (LOG_BUFFER_SIZE - log_tail + log_head);
    spin_unlock_irqrestore(&log_lock, flags);
    return len;
}

void log_read(char *buf, int len, int *out_len) {
    if (!buf || len <= 0) {
        if (out_len) *out_len = 0;
        return;
    }
    
    unsigned long flags;
    spin_lock_irqsave(&log_lock, &flags);
    
    int available = (log_head >= log_tail) ? (log_head - log_tail) : (LOG_BUFFER_SIZE - log_tail + log_head);
    int to_read = len < available ? len : available;
    
    for (int i = 0; i < to_read; i++) {
        buf[i] = log_buffer[(log_tail + i) % LOG_BUFFER_SIZE];
    }
    
    if (out_len) *out_len = to_read;
    spin_unlock_irqrestore(&log_lock, flags);
}
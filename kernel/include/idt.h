#ifndef IDT_H
#define IDT_H

#include <stdint.h>
#include <stdbool.h>
#include <workqueue.h>

// IDT entry structure (16 bytes on x86_64)
struct idt_entry {
    uint16_t offset_low;      // Offset bits 0..15
    uint16_t selector;        // Code segment selector (0x08 in our GDT)
    uint8_t  ist;             // Interrupt Stack Table offset (0 = unused)
    uint8_t  types_attr;      // Type and attributes (e.g. 0x8E = 64-bit interrupt gate)
    uint16_t offset_mid;      // Offset bits 16..31
    uint32_t offset_high;     // Offset bits 32..63
    uint32_t reserved;        // Reserved, must be 0
} __attribute__((packed));

// IDT pointer structure for LIDT instruction
struct idt_ptr {
    uint16_t limit;           // IDT size - 1
    uint64_t base;            // Absolute address of first IDT entry
} __attribute__((packed));

// Register frame layout pushed to stack before calling C handler
struct interrupt_frame {
    // Manually pushed in assembly (r15 down to rax)
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    
    // Pushed in Assembly handler to identify the interrupt
    uint64_t int_no;
    uint64_t err_code;
    
    // Automatically pushed by x86_64 CPU on interrupt
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

// Typedef for a custom interrupt handler function
typedef void (*interrupt_handler_t)(struct interrupt_frame* frame);

// Initialize IDT and setup PIC
void idt_init(void);

// Configure a specific IDT gate
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist);

// Register a C-level driver handler for an interrupt (e.g. PIT or Keyboard)
void interrupts_register_handler(uint8_t num, interrupt_handler_t handler);

// Dynamic IRQ registration with threading support
int request_irq(uint8_t irq, interrupt_handler_t handler, bool threaded, const char *name, void *dev_id);
void free_irq(uint8_t irq, void *dev_id);

// MSI-X vector registration (vectors 48..255, for APIC-delivered device interrupts)
int request_msi_irq(uint8_t vector, interrupt_handler_t handler, bool threaded,
                     const char *name, void *dev_id);
void free_msi_irq(uint8_t vector, void *dev_id);

#endif

#include <idt.h>
#include <io.h>
#include <screen.h>
#include <kheap.h>
#include <workqueue.h>
#include <spinlock.h>
#include <panic.h>

// IDT table containing 256 descriptors
static struct idt_entry idt[256];
static struct idt_ptr   idt_pointer;

// Array of registered C-level interrupt handlers
static interrupt_handler_t interrupt_handlers[256];

typedef struct irq_action {
    interrupt_handler_t handler;
    void *dev_id;
    struct irq_action *next;
    char name[32];
    bool threaded;
    struct work_struct work;
} irq_action_t;

static irq_action_t *irq_actions[16] = {0};
static spinlock_irq_t irq_lock = { SPINLOCK_INIT, 0 };

static void irq_thread_work(struct work_struct *work);

static void irq_dispatcher(struct interrupt_frame *frame);

static void irq_thread_work(struct work_struct *work) {
    irq_action_t *action = (irq_action_t *)((char *)work - offsetof(irq_action_t, work));
    if (action->handler) {
        struct interrupt_frame dummy_frame = {0};
        action->handler(&dummy_frame);
    }
}

// Declare external low-level assembly ISR handlers (CPUID Exceptions 0..31)
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);

// Syscall entry point (int 0x80)
extern void syscall_entry(void);

// Declare external low-level assembly IRQ handlers (Hardware IRQs 0..15 remapped to 32..47)
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

// CPU Exception messages
static const char* exception_messages[] = {
    "Division By Zero",
    "Debug Exception",
    "Non Maskable Interrupt",
    "Breakpoint Exception",
    "Into Detected Overflow",
    "Out of Bounds Exception",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt Exception",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Security Exception", "Reserved"
};

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist) {
    idt[num].offset_low  = (uint16_t)(base & 0xFFFF);
    idt[num].selector    = sel;
    idt[num].ist         = ist;
    idt[num].types_attr  = flags;
    idt[num].offset_mid  = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].offset_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt[num].reserved    = 0;
}

// Remaps the master and slave PICs to vector offsets 32 and 40
static void pic_remap(void) {
    uint8_t a1 = inb(0x21);
    uint8_t a2 = inb(0xA1);

    outb(0x20, 0x11); // Start PIC initialization (ICW1)
    io_wait();
    outb(0xA0, 0x11);
    io_wait();

    outb(0x21, 0x20); // ICW2: Master PIC vector offset to 32 (0x20)
    io_wait();
    outb(0xA1, 0x28); // ICW2: Slave PIC vector offset to 40 (0x28)
    io_wait();

    outb(0x21, 0x04); // ICW3: Tell Master PIC there is a slave at IRQ2
    io_wait();
    outb(0xA1, 0x02); // ICW3: Tell Slave PIC its cascade identity (2)
    io_wait();

    outb(0x21, 0x01); // ICW4: Set 8086 mode
    io_wait();
    outb(0xA1, 0x01);
    io_wait();

    // Restore saved masks
    outb(0x21, a1);
    outb(0xA1, a2);
}

void idt_init(void) {
    // 1. Remap the Programmable Interrupt Controller
    pic_remap();

    // 2. Set up exception handlers (0-31)
    idt_set_gate(0,  (uint64_t)isr0,  0x08, 0x8E, 0); idt_set_gate(1,  (uint64_t)isr1,  0x08, 0x8E, 0);
    idt_set_gate(2,  (uint64_t)isr2,  0x08, 0x8E, 0); idt_set_gate(3,  (uint64_t)isr3,  0x08, 0x8E, 0);
    idt_set_gate(4,  (uint64_t)isr4,  0x08, 0x8E, 0); idt_set_gate(5,  (uint64_t)isr5,  0x08, 0x8E, 0);
    idt_set_gate(6,  (uint64_t)isr6,  0x08, 0x8E, 0); idt_set_gate(7,  (uint64_t)isr7,  0x08, 0x8E, 0);
    idt_set_gate(8,  (uint64_t)isr8,  0x08, 0x8E, 1); idt_set_gate(9,  (uint64_t)isr9,  0x08, 0x8E, 0);
    idt_set_gate(10, (uint64_t)isr10, 0x08, 0x8E, 0); idt_set_gate(11, (uint64_t)isr11, 0x08, 0x8E, 0);
    idt_set_gate(12, (uint64_t)isr12, 0x08, 0x8E, 0); idt_set_gate(13, (uint64_t)isr13, 0x08, 0x8E, 0);
    idt_set_gate(14, (uint64_t)isr14, 0x08, 0x8E, 0); idt_set_gate(15, (uint64_t)isr15, 0x08, 0x8E, 0);
    idt_set_gate(16, (uint64_t)isr16, 0x08, 0x8E, 0); idt_set_gate(17, (uint64_t)isr17, 0x08, 0x8E, 0);
    idt_set_gate(18, (uint64_t)isr18, 0x08, 0x8E, 0); idt_set_gate(19, (uint64_t)isr19, 0x08, 0x8E, 0);
    idt_set_gate(20, (uint64_t)isr20, 0x08, 0x8E, 0); idt_set_gate(21, (uint64_t)isr21, 0x08, 0x8E, 0);
    idt_set_gate(22, (uint64_t)isr22, 0x08, 0x8E, 0); idt_set_gate(23, (uint64_t)isr23, 0x08, 0x8E, 0);
    idt_set_gate(24, (uint64_t)isr24, 0x08, 0x8E, 0); idt_set_gate(25, (uint64_t)isr25, 0x08, 0x8E, 0);
    idt_set_gate(26, (uint64_t)isr26, 0x08, 0x8E, 0); idt_set_gate(27, (uint64_t)isr27, 0x08, 0x8E, 0);
    idt_set_gate(28, (uint64_t)isr28, 0x08, 0x8E, 0); idt_set_gate(29, (uint64_t)isr29, 0x08, 0x8E, 0);
    idt_set_gate(30, (uint64_t)isr30, 0x08, 0x8E, 0); idt_set_gate(31, (uint64_t)isr31, 0x08, 0x8E, 0);

    // 3. Set up hardware IRQ handlers (32-47)
    idt_set_gate(32, (uint64_t)irq0,  0x08, 0x8E, 0); idt_set_gate(33, (uint64_t)irq1,  0x08, 0x8E, 0);
    idt_set_gate(34, (uint64_t)irq2,  0x08, 0x8E, 0); idt_set_gate(35, (uint64_t)irq3,  0x08, 0x8E, 0);
    idt_set_gate(36, (uint64_t)irq4,  0x08, 0x8E, 0); idt_set_gate(37, (uint64_t)irq5,  0x08, 0x8E, 0);
    idt_set_gate(38, (uint64_t)irq6,  0x08, 0x8E, 0); idt_set_gate(39, (uint64_t)irq7,  0x08, 0x8E, 0);
    idt_set_gate(40, (uint64_t)irq8,  0x08, 0x8E, 0); idt_set_gate(41, (uint64_t)irq9,  0x08, 0x8E, 0);
    idt_set_gate(42, (uint64_t)irq10, 0x08, 0x8E, 0); idt_set_gate(43, (uint64_t)irq11, 0x08, 0x8E, 0);
    idt_set_gate(44, (uint64_t)irq12, 0x08, 0x8E, 0); idt_set_gate(45, (uint64_t)irq13, 0x08, 0x8E, 0);
    idt_set_gate(46, (uint64_t)irq14, 0x08, 0x8E, 0); idt_set_gate(47, (uint64_t)irq15, 0x08, 0x8E, 0);

    // 4. Syscall entry (int 0x80 = vector 128, DPL=3 for user access)
    idt_set_gate(0x80, (uint64_t)syscall_entry, 0x08, 0xEE, 0);

    // 5. Load the IDT pointer into CPU register
    idt_pointer.limit = (sizeof(struct idt_entry) * 256) - 1;
    idt_pointer.base  = (uint64_t)&idt;
    __asm__ volatile("lidt %0" : : "m"(idt_pointer));

    // Clear registered C-level handlers
    for (int i = 0; i < 256; i++) {
        interrupt_handlers[i] = 0;
    }
}

void interrupts_register_handler(uint8_t num, interrupt_handler_t handler) {
    interrupt_handlers[num] = handler;
}

int request_irq(uint8_t irq, interrupt_handler_t handler, bool threaded, const char *name, void *dev_id) {
    if (irq >= 16) return -1;
    
    unsigned long flags;
    spin_lock_irqsave(&irq_lock, &flags);

    irq_action_t *action = (irq_action_t *)kmalloc(sizeof(irq_action_t));
    if (!action) {
        spin_unlock_irqrestore(&irq_lock, flags);
        return -1;
    }

    action->handler = handler;
    action->dev_id = dev_id;
    action->threaded = threaded;
    action->next = NULL;
    if (name) {
        int i = 0;
        while (name[i] && i < 31) {
            action->name[i] = name[i];
            i++;
        }
        action->name[i] = '\0';
    } else {
        action->name[0] = '\0';
    }

    if (threaded) {
        INIT_WORK(&action->work, irq_thread_work);
    }

    if (!irq_actions[irq]) {
        irq_actions[irq] = action;
    } else {
        irq_action_t *curr = irq_actions[irq];
        while (curr->next) curr = curr->next;
        curr->next = action;
    }

    uint8_t vector = irq + 32;
    interrupt_handlers[vector] = irq_dispatcher;

    if (irq >= 8) {
        uint8_t mask = inb(0xA1);
        outb(0xA1, mask & ~(1 << (irq - 8)));
    } else {
        uint8_t mask = inb(0x21);
        outb(0x21, mask & ~(1 << irq));
    }

    spin_unlock_irqrestore(&irq_lock, flags);
    return 0;
}

void free_irq(uint8_t irq, void *dev_id) {
    if (irq >= 16) return;
    
    unsigned long flags;
    spin_lock_irqsave(&irq_lock, &flags);

    irq_action_t **curr = &irq_actions[irq];
    while (*curr) {
        if ((*curr)->dev_id == dev_id) {
            irq_action_t *to_free = *curr;
            *curr = (*curr)->next;
            kfree(to_free);
            break;
        }
        curr = &(*curr)->next;
    }

    if (!irq_actions[irq]) {
        uint8_t vector = irq + 32;
        interrupt_handlers[vector] = 0;

        if (irq >= 8) {
            uint8_t mask = inb(0xA1);
            outb(0xA1, mask | (1 << (irq - 8)));
        } else {
            uint8_t mask = inb(0x21);
            outb(0x21, mask | (1 << irq));
        }
    }

    spin_unlock_irqrestore(&irq_lock, flags);
}

void irq_dispatcher(struct interrupt_frame *frame) {
    uint8_t irq = frame->int_no - 32;
    if (irq >= 16) return;

    irq_action_t *action = irq_actions[irq];
    while (action) {
        if (action->threaded) {
            queue_work(system_wq, &action->work);
        } else if (action->handler) {
            action->handler(frame);
        }
        action = action->next;
    }

    if (irq >= 8) {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
}

// Common C-level interrupt dispatcher called by Assembly handlers
void interrupt_handler(struct interrupt_frame* frame) {
    // 1. Handle CPU exceptions (0..31)
    if (frame->int_no < 32) {
        dump_registers(frame);
        PANIC("CPU Exception %d: %s", frame->int_no, exception_messages[frame->int_no]);
    }

    // 2. Handle registered hardware drivers (32..47)
    if (interrupt_handlers[frame->int_no] != 0) {
        interrupt_handlers[frame->int_no](frame);
    }

    // 3. Check for pending signals before returning to userspace
    if (frame->cs == 0x1B || frame->cs == 0x23) { /* User mode CS (Ring 3) */
        extern void do_signal(struct interrupt_frame *frame);
        do_signal(frame);
    }
}

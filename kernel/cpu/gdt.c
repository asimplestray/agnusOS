/*
 * GDT — Global Descriptor Table + TSS (64-bit AgnusOS)
 *
 * We install 6 descriptors:
 *   0: null
 *   1: kernel code  (CS=0x08, Ring 0, 64-bit)
 *   2: kernel data  (DS=0x10, Ring 0)
 *   3: user data    (DS=0x1B, Ring 3)
 *   4: user code    (CS=0x23, Ring 3, 64-bit)
 *   5+6: TSS descriptor (16-byte system descriptor occupies 2 slots)
 */

#include <gdt.h>
#include <stdint.h>
#include <stddef.h>
#include <kheap.h>

/* TSS instance */
tss64_t kernel_tss;

/* IST stack for double fault (4KB) */
uint8_t ist_double_fault_stack[4096] __attribute__((aligned(16)));

/* -------------------------------------------------------------------------
 * GDT entry layouts
 * ---------------------------------------------------------------------- */

/* 8-byte code/data descriptor */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;      /* P | DPL | S | type */
    uint8_t  granularity; /* G | DB | L | AVL | limit_high */
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

/* 16-byte TSS/LDT system descriptor */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;      /* 0x89 = Present | TSS64 available */
    uint8_t  granularity; /* upper limit bits + flags */
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed)) gdt_tss_entry_t;

/* Full GDT: 5 regular + 1 TSS (which is 16 bytes = 2 slots) */
static struct {
    gdt_entry_t     null;
    gdt_entry_t     kernel_cs;
    gdt_entry_t     kernel_ds;
    gdt_entry_t     user_ds;
    gdt_entry_t     user_cs;
    gdt_tss_entry_t tss;
} __attribute__((packed)) gdt;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr_t;

static gdtr_t gdtr;

/* -------------------------------------------------------------------------
 * Descriptor builders
 * ---------------------------------------------------------------------- */

static void gdt_set_entry(gdt_entry_t *e, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran)
{
    e->limit_low  = (uint16_t)(limit & 0xFFFF);
    e->base_low   = (uint16_t)(base  & 0xFFFF);
    e->base_mid   = (uint8_t)((base  >> 16) & 0xFF);
    e->access     = access;
    e->granularity = (gran & 0xF0) | ((limit >> 16) & 0x0F);
    e->base_high  = (uint8_t)((base >> 24) & 0xFF);
}

static void gdt_set_tss(gdt_tss_entry_t *e, uint64_t base, uint32_t limit)
{
    e->limit_low  = (uint16_t)(limit & 0xFFFF);
    e->base_low   = (uint16_t)(base  & 0xFFFF);
    e->base_mid   = (uint8_t)((base >> 16) & 0xFF);
    e->access     = 0x89; /* Present | 64-bit TSS available */
    e->granularity = 0x00;
    e->base_high  = (uint8_t)((base >> 24) & 0xFF);
    e->base_upper = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    e->reserved   = 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void gdt_init(void)
{
    /* 0: null */
    gdt_set_entry(&gdt.null,      0, 0,          0x00, 0x00);

    /* 1: kernel code — Ring 0, 64-bit (L=1), execute/read */
    gdt_set_entry(&gdt.kernel_cs, 0, 0xFFFFF,    0x9A, 0xA0);

    /* 2: kernel data — Ring 0, 32/64-bit, read/write */
    gdt_set_entry(&gdt.kernel_ds, 0, 0xFFFFF,    0x92, 0xC0);

    /* 3: user data — Ring 3, DPL=3 (access=0xF2) */
    gdt_set_entry(&gdt.user_ds,   0, 0xFFFFF,    0xF2, 0xC0);

    /* 4: user code — Ring 3, 64-bit (L=1), DPL=3 (access=0xFA) */
    gdt_set_entry(&gdt.user_cs,   0, 0xFFFFF,    0xFA, 0xA0);

    /* TSS */
    uint64_t tss_base  = (uint64_t)&kernel_tss;
    uint32_t tss_limit = (uint32_t)(sizeof(tss64_t) - 1);
    gdt_set_tss(&gdt.tss, tss_base, tss_limit);

    /* Zero the TSS */
    uint8_t *tp = (uint8_t *)&kernel_tss;
    for (uint32_t i = 0; i < sizeof(tss64_t); i++) tp[i] = 0;
    kernel_tss.iopb_offset = (uint16_t)sizeof(tss64_t);

    /* Set IST1 for double fault handler */
    kernel_tss.ist1 = (uint64_t)&ist_double_fault_stack[4096];  // Top of stack

    /* Load the GDT */
    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint64_t)&gdt;
    gdt_flush((uint64_t)&gdtr);

    /* Load the TSS (selector 0x28 = index 5, TI=0, RPL=0) */
    tss_flush();
}

void tss_set_kernel_stack(uint64_t rsp0)
{
    kernel_tss.rsp0 = rsp0;
}

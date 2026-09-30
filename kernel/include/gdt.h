#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/*
 * GDT Segment Selectors
 *
 * Selector encoding: index*8 | TI | RPL
 *   TI=0 (GDT), RPL=0 (Ring 0) for kernel segments
 *   TI=0 (GDT), RPL=3 (Ring 3) for user segments
 */
#define GDT_NULL        0x00   /* Null descriptor */
#define GDT_KERNEL_CS   0x08   /* Kernel Code  (Ring 0, index 1) */
#define GDT_KERNEL_DS   0x10   /* Kernel Data  (Ring 0, index 2) */
#define GDT_USER_DS     0x18   /* User   Data  (Ring 3, index 3, RPL=3 → 0x1B) */
#define GDT_USER_CS     0x20   /* User   Code  (Ring 3, index 4, RPL=3 → 0x23) */
#define GDT_TSS_SEL     0x28   /* TSS descriptor (index 5) */

/* Convenience: selectors with RPL=3 for iretq frames */
#define GDT_USER_DS_RPL3  (GDT_USER_DS | 3)   /* 0x1B */
#define GDT_USER_CS_RPL3  (GDT_USER_CS | 3)   /* 0x23 */

/* 64-bit TSS (104 bytes per the x86_64 spec) */
typedef struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;   /* Kernel stack pointer used on interrupt from Ring 3 */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;   /* Interrupt Stack Table entries */
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss64_t;

#define IST_DFAULT_STACK  1  /* IST index 1 for double fault */

extern tss64_t kernel_tss;
extern uint8_t ist_double_fault_stack[4096];

/* Initialize GDT and TSS, load via lgdt/ltr */
void gdt_init(void);

/* Update RSP0 in the TSS — call before every iretq to userspace */
void tss_set_kernel_stack(uint64_t rsp0);
/* Garante EFER.NXE=1 (cobre boot que pula o boot.asm, ex. Limine direto
 * em 64 bits). Sem NXE, o bit NX que o kernel usa em páginas USER é
 * reservado e gera #PF(RSVD). Idempotente; PANIC se a CPU não tem NX. */
void cpu_enable_nxe(void);

/* Endurece a execução Ring 0 (P0 §3, passo 1 — sem mudar layout):
 * - CR0.WP=1: supervisor passa a respeitar páginas read-only (hoje todas
 *   as páginas do kernel ainda são RW, então é no-op comportamental e
 *   guardrail para o futuro texto read-only).
 * - CR4.SMEP=1 (se cpuid.7:EBX[7]): Ring 0 nunca executa página USER
 *   (iret para Ring 3 não é afetado). O kernel nunca executa memória
 *   de usuário em modo supervisor, então é seguro ligar.
 * - SMAP é só detectado e reportado: ligar sem auditoria completa dos
 *   caminhos internos (além da borda uaccess) arrisca #PF no boot.
 * Idempotente; só liga bits, nunca desliga. */
void cpu_harden(void);

/* Assembly helpers (defined in gdt.asm) */
extern void gdt_flush(uint64_t gdt_ptr);
extern void tss_flush(void);

/* Assembly helper to jump into Ring 3 */
extern void jump_to_usermode(uint64_t entry, uint64_t user_rsp);

/* Assembly helper to jump into Ring 3 with argc/argv/envp */
extern void jump_to_usermode_with_args(uint64_t entry, uint64_t user_rsp,
                                       uint64_t argc, uint64_t argv, uint64_t envp);

#endif

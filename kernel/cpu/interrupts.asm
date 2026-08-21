bits 64

extern interrupt_handler
extern vmm_page_fault_handler
extern current
extern schedule
extern need_resched

; task_struct.rsp offset (see kernel/include/task.h)
task_struct_rsp equ 96

section .text

; Common stub that saves registers, calls the C handler, and restores state
irq_common_stub:
    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; The System V AMD64 ABI passes the first argument in RDI
    ; Pass the stack pointer (which is the address of the interrupt frame)
    mov rdi, rsp

    ; Call our C dispatcher
    call interrupt_handler

    ; Fall through to common restore with preemption check
    jmp irq_common_restore

; Common restore path with preemptive context switch check
irq_common_restore:
    ; --- Preemptive context switch check ---
    ; If need_resched was set by the C handler (timer quantum expired),
    ; save the current task's frame pointer, pick the next task, and
    ; switch to its frame. The register values live inside each task's
    ; own kernel stack frame, so we only ever swap the stack pointer.
    cmp qword [rel need_resched], 0
    je .restore_regs

    ; Clear the flag so we don't reschedule again on this IRQ return
    mov qword [rel need_resched], 0

    ; Save this task's frame pointer (rsp points at its saved R15 slot)
    mov rax, [rel current]
    test rax, rax
    jz .restore_regs
    mov [rax + task_struct_rsp], rsp

    ; Call the scheduler: pure selection, updates `current`
    call schedule

    ; Load the next task's frame pointer
    mov rax, [rel current]
    test rax, rax
    jz .restore_regs
    mov rsp, [rax + task_struct_rsp]

.restore_regs:
    ; Restore registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Pop interrupt number and error code
    add rsp, 16

    ; Return from interrupt (64-bit far return)
    iretq

; Macro for Interrupt Service Routines that DO NOT push an error code
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push qword 0        ; Push dummy error code
    push qword %1       ; Push interrupt number
    jmp irq_common_stub
%endmacro

; Macro for Interrupt Service Routines that DO push an error code
%macro ISR_ERRCODE 1
global isr%1
isr%1:
    ; Error code is already pushed by CPU
    push qword %1       ; Push interrupt number
    jmp irq_common_stub
%endmacro

; Page Fault handler (vector 14) - needs CR2 (fault address)
global isr14
isr14:
    ; Error code is already pushed by CPU
    ; Read CR2 (faulting address) BEFORE it gets clobbered
    mov rax, cr2
    push rax            ; Push fault address as 3rd argument
    push qword 14       ; Push interrupt number
    jmp irq_common_stub_pf

; Common stub for page fault - 3 args: frame, error_code, fault_addr
irq_common_stub_pf:
    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Stack now: R15..RAX, fault_addr, err_code, int_no
    ; Arguments for C handler: RDI=frame, RSI=err_code, RDX=fault_addr
    mov rdi, rsp                    ; frame pointer (R15 slot)
    mov rsi, [rsp + 16*8]           ; error_code
    mov rdx, [rsp + 17*8]           ; fault_addr

    ; Call page fault handler
    call vmm_page_fault_handler

    jmp irq_common_restore

; Macro for Hardware Interrupt Requests
%macro IRQ 2
global irq%1
irq%1:
    push qword 0        ; Push dummy error code
    push qword %2       ; Push remapped interrupt vector (32 + IRQ)
    jmp irq_common_stub
%endmacro

; Define Exceptions (0..31)
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8  ; Double Fault
ISR_NOERRCODE 9
ISR_ERRCODE   10 ; Invalid TSS
ISR_ERRCODE   11 ; Segment Not Present
ISR_ERRCODE   12 ; Stack Fault
ISR_ERRCODE   13 ; General Protection Fault
; ISR_ERRCODE   14 ; Page Fault - CUSTOM HANDLER (see isr14 above)
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17 ; Alignment Check
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30 ; Security Exception
ISR_NOERRCODE 31

; Define Hardware IRQs (32..47)
IRQ 0,  32  ; System Timer
IRQ 1,  33  ; PS/2 Keyboard
IRQ 2,  34  ; Cascade (used internally)
IRQ 3,  35  ; COM2 (serial port)
IRQ 4,  36  ; COM1
IRQ 5,  37  ; LPT2
IRQ 6,  38  ; Floppy Controller
IRQ 7,  39  ; LPT1
IRQ 8,  40  ; Real Time Clock
IRQ 9,  41  ; ACPI
IRQ 10, 42  ; Available
IRQ 11, 43  ; Available
IRQ 12, 44  ; PS/2 Mouse
IRQ 13, 45  ; Coprocessor
IRQ 14, 46  ; Primary ATA (Hard Disk)
IRQ 15, 47  ; Secondary ATA

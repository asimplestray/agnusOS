bits 64

; -------------------------------------------------------------------------
; gdt_flush — reload segment registers after installing a new GDT
;
; void gdt_flush(uint64_t gdtr_ptr);
; RDI = pointer to GDTR struct { uint16_t limit; uint64_t base; }
; -------------------------------------------------------------------------
global gdt_flush
gdt_flush:
    lgdt [rdi]

    ; Reload CS via a far return trick
    push qword 0x08        ; new CS = kernel code selector
    lea  rax, [rel .after]
    push rax
    retfq
.after:
    ; Reload data segment registers
    mov ax, 0x10           ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

; -------------------------------------------------------------------------
; tss_flush — load the TSS selector into TR
; -------------------------------------------------------------------------
global tss_flush
tss_flush:
    mov ax, 0x28           ; TSS selector (index 5, RPL=0)
    ltr ax
    ret

; -------------------------------------------------------------------------
; jump_to_usermode — perform iretq into Ring 3
;
; void jump_to_usermode(uint64_t entry, uint64_t user_rsp);
; RDI = entry point (RIP for Ring-3 process)
; RSI = user stack pointer
;
; iretq pops: RIP, CS, RFLAGS, RSP, SS  (in that order, low→high addr)
; -------------------------------------------------------------------------
global jump_to_usermode
extern tss_set_kernel_stack
extern current                 ; task_struct_t *current

jump_to_usermode:
    ; RDI = entry, RSI = user_rsp
    mov r12, rdi               ; save entry
    mov r13, rsi               ; save user_rsp

    ; Update TSS RSP0 with current kernel stack
    mov  rax, [rel current]
    test rax, rax
    jz   .no_tss
    mov  rdi, [rax + 80]       ; task_struct_t.kernel_stack  (offset 80)
    call tss_set_kernel_stack
.no_tss:

    ; Build iretq frame on the stack
    ; Ring-3 selectors:
    ;   SS  = GDT_USER_DS_RPL3 = 0x1B
    ;   CS  = GDT_USER_CS_RPL3 = 0x23
    mov  ax, 0x1B
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    push qword 0x1B            ; SS  (user data, RPL=3)
    push r13                   ; RSP (user stack)
    pushfq
    pop  rax
    or   rax, 0x200            ; ensure IF is set in RFLAGS
    push rax                   ; RFLAGS
    push qword 0x23            ; CS  (user code, RPL=3)
    push r12                   ; RIP (entry point)

    ; Zero general-purpose registers for a clean start
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor rbp, rbp
    xor r8,  r8
    xor r9,  r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    iretq

; -------------------------------------------------------------------------
; jump_to_usermode_with_args — perform iretq into Ring 3 with argc/argv/envp
;
; void jump_to_usermode_with_args(uint64_t entry, uint64_t user_rsp,
;                                 uint64_t argc, uint64_t argv, uint64_t envp);
; RDI = entry point (RIP for Ring-3 process)
; RSI = user stack pointer (points to argc)
; RDX = argc
; RCX = argv pointer
; R8  = envp pointer
;
; Sets up registers per SysV ABI:
;   RDI = argc
;   RSI = argv
;   RDX = envp
; -------------------------------------------------------------------------
global jump_to_usermode_with_args

jump_to_usermode_with_args:
    ; RDI = entry, RSI = user_rsp, RDX = argc, RCX = argv, R8 = envp
    mov r12, rdi               ; save entry
    mov r13, rsi               ; save user_rsp
    mov r14, rdx               ; save argc
    mov r15, rcx               ; save argv
    mov r10, r8                ; save envp

    ; Update TSS RSP0 with current kernel stack
    mov  rax, [rel current]
    test rax, rax
    jz   .no_tss2
    mov  rdi, [rax + 80]       ; task_struct_t.kernel_stack  (offset 80)
    call tss_set_kernel_stack
.no_tss2:

    ; Build iretq frame on the stack
    ; Ring-3 selectors:
    ;   SS  = GDT_USER_DS_RPL3 = 0x1B
    ;   CS  = GDT_USER_CS_RPL3 = 0x23
    mov  ax, 0x1B
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    push qword 0x1B            ; SS  (user data, RPL=3)
    push r13                   ; RSP (user stack - points to argc)
    pushfq
    pop  rax
    or   rax, 0x200            ; ensure IF is set in RFLAGS
    push rax                   ; RFLAGS
    push qword 0x23            ; CS  (user code, RPL=3)
    push r12                   ; RIP (entry point)

    ; Set up SysV ABI registers: RDI=argc, RSI=argv, RDX=envp
    mov rdi, r14               ; argc
    mov rsi, r15               ; argv
    mov rdx, r10               ; envp

    ; Zero other registers for a clean start
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rbp, rbp
    xor r8,  r8
    xor r9,  r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    iretq

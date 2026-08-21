section .text
global syscall_entry
extern syscall_handler

syscall_entry:
    cli

    ; CPU has already pushed: rip, cs, rflags, rsp, ss (in that order, ss at top)
    ; We need to build interrupt_frame:
    ;   r15..rax, int_no, err_code, rip, cs, rflags, rsp, ss
    ; So push err_code (0), int_no (0x80), then r15..rax

    push qword 0           ; dummy error code
    push qword 0x80        ; interrupt number (vector 0x80)

    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push rbx
    push rdx
    push rcx
    push rax

    ; RSP now points to interrupt_frame (r15 slot)
    ; Pass frame pointer to syscall_handler in RDI
    mov rdi, rsp

    ; Syscall number was in RAX before pushes, now saved on stack at offset 15*8
    ; Extract it for syscall_handler
    mov rax, [rsp + 15*8]  ; saved rax = syscall number
    mov rsi, [rsp + 14*8]  ; saved rcx = arg1
    mov rdx, [rsp + 13*8]  ; saved rdx = arg2
    mov rcx, [rsp + 12*8]  ; saved rbx = arg3
    mov r8,  [rsp + 11*8]  ; saved rbp = arg4
    mov r9,  [rsp + 10*8]  ; saved rsi = arg5
    ; r10 = [rsp + 9*8]   ; saved rdi = arg6 (not used)

    call syscall_handler

    ; syscall_handler may have modified the frame (e.g., sigreturn)
    ; Restore registers from frame
    pop rax
    pop rcx
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    ; Pop int_no and err_code
    add rsp, 16

    ; Stack now has CPU-pushed: rip, cs, rflags, rsp, ss
    sti
    iretq
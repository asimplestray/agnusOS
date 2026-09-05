bits 64

extern interrupt_handler
extern vmm_page_fault_handler
extern need_resched

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

; Common restore path
; NOTE: Preemption is handled exclusively by schedule() → context_switch().
; The timer ISR sets need_resched=1; the idle loop and kworker check it
; after waking from hlt and call schedule() themselves.
; We do NOT context-switch here — it would conflict with
; context_switch()'s ret-based stack swap (different frame layouts).
irq_common_restore:

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

; Define generic MSI-X ISR stubs (48..255) for APIC-delivered interrupts
; Each stub is exactly 12 bytes: push imm8, push imm32, jmp rel32
%macro MSI_IRQ_UNIFORM 1
global msirq%1
msirq%1:
    push byte 0                 ; 6A 00 (2 bytes) — dummy error code
    db 0x68                     ; push dword imm32 opcode (force 5-byte form)
    dd %1                       ; push vector number as imm32
    jmp irq_common_stub         ; E9 rel32 (5 bytes) = 12 total
%endmacro

MSI_IRQ_UNIFORM 48
MSI_IRQ_UNIFORM 49
MSI_IRQ_UNIFORM 50
MSI_IRQ_UNIFORM 51
MSI_IRQ_UNIFORM 52
MSI_IRQ_UNIFORM 53
MSI_IRQ_UNIFORM 54
MSI_IRQ_UNIFORM 55
MSI_IRQ_UNIFORM 56
MSI_IRQ_UNIFORM 57
MSI_IRQ_UNIFORM 58
MSI_IRQ_UNIFORM 59
MSI_IRQ_UNIFORM 60
MSI_IRQ_UNIFORM 61
MSI_IRQ_UNIFORM 62
MSI_IRQ_UNIFORM 63
MSI_IRQ_UNIFORM 64
MSI_IRQ_UNIFORM 65
MSI_IRQ_UNIFORM 66
MSI_IRQ_UNIFORM 67
MSI_IRQ_UNIFORM 68
MSI_IRQ_UNIFORM 69
MSI_IRQ_UNIFORM 70
MSI_IRQ_UNIFORM 71
MSI_IRQ_UNIFORM 72
MSI_IRQ_UNIFORM 73
MSI_IRQ_UNIFORM 74
MSI_IRQ_UNIFORM 75
MSI_IRQ_UNIFORM 76
MSI_IRQ_UNIFORM 77
MSI_IRQ_UNIFORM 78
MSI_IRQ_UNIFORM 79
MSI_IRQ_UNIFORM 80
MSI_IRQ_UNIFORM 81
MSI_IRQ_UNIFORM 82
MSI_IRQ_UNIFORM 83
MSI_IRQ_UNIFORM 84
MSI_IRQ_UNIFORM 85
MSI_IRQ_UNIFORM 86
MSI_IRQ_UNIFORM 87
MSI_IRQ_UNIFORM 88
MSI_IRQ_UNIFORM 89
MSI_IRQ_UNIFORM 90
MSI_IRQ_UNIFORM 91
MSI_IRQ_UNIFORM 92
MSI_IRQ_UNIFORM 93
MSI_IRQ_UNIFORM 94
MSI_IRQ_UNIFORM 95
MSI_IRQ_UNIFORM 96
MSI_IRQ_UNIFORM 97
MSI_IRQ_UNIFORM 98
MSI_IRQ_UNIFORM 99
MSI_IRQ_UNIFORM 100
MSI_IRQ_UNIFORM 101
MSI_IRQ_UNIFORM 102
MSI_IRQ_UNIFORM 103
MSI_IRQ_UNIFORM 104
MSI_IRQ_UNIFORM 105
MSI_IRQ_UNIFORM 106
MSI_IRQ_UNIFORM 107
MSI_IRQ_UNIFORM 108
MSI_IRQ_UNIFORM 109
MSI_IRQ_UNIFORM 110
MSI_IRQ_UNIFORM 111
MSI_IRQ_UNIFORM 112
MSI_IRQ_UNIFORM 113
MSI_IRQ_UNIFORM 114
MSI_IRQ_UNIFORM 115
MSI_IRQ_UNIFORM 116
MSI_IRQ_UNIFORM 117
MSI_IRQ_UNIFORM 118
MSI_IRQ_UNIFORM 119
MSI_IRQ_UNIFORM 120
MSI_IRQ_UNIFORM 121
MSI_IRQ_UNIFORM 122
MSI_IRQ_UNIFORM 123
MSI_IRQ_UNIFORM 124
MSI_IRQ_UNIFORM 125
MSI_IRQ_UNIFORM 126
MSI_IRQ_UNIFORM 127
MSI_IRQ_UNIFORM 128
MSI_IRQ_UNIFORM 129
MSI_IRQ_UNIFORM 130
MSI_IRQ_UNIFORM 131
MSI_IRQ_UNIFORM 132
MSI_IRQ_UNIFORM 133
MSI_IRQ_UNIFORM 134
MSI_IRQ_UNIFORM 135
MSI_IRQ_UNIFORM 136
MSI_IRQ_UNIFORM 137
MSI_IRQ_UNIFORM 138
MSI_IRQ_UNIFORM 139
MSI_IRQ_UNIFORM 140
MSI_IRQ_UNIFORM 141
MSI_IRQ_UNIFORM 142
MSI_IRQ_UNIFORM 143
MSI_IRQ_UNIFORM 144
MSI_IRQ_UNIFORM 145
MSI_IRQ_UNIFORM 146
MSI_IRQ_UNIFORM 147
MSI_IRQ_UNIFORM 148
MSI_IRQ_UNIFORM 149
MSI_IRQ_UNIFORM 150
MSI_IRQ_UNIFORM 151
MSI_IRQ_UNIFORM 152
MSI_IRQ_UNIFORM 153
MSI_IRQ_UNIFORM 154
MSI_IRQ_UNIFORM 155
MSI_IRQ_UNIFORM 156
MSI_IRQ_UNIFORM 157
MSI_IRQ_UNIFORM 158
MSI_IRQ_UNIFORM 159
MSI_IRQ_UNIFORM 160
MSI_IRQ_UNIFORM 161
MSI_IRQ_UNIFORM 162
MSI_IRQ_UNIFORM 163
MSI_IRQ_UNIFORM 164
MSI_IRQ_UNIFORM 165
MSI_IRQ_UNIFORM 166
MSI_IRQ_UNIFORM 167
MSI_IRQ_UNIFORM 168
MSI_IRQ_UNIFORM 169
MSI_IRQ_UNIFORM 170
MSI_IRQ_UNIFORM 171
MSI_IRQ_UNIFORM 172
MSI_IRQ_UNIFORM 173
MSI_IRQ_UNIFORM 174
MSI_IRQ_UNIFORM 175
MSI_IRQ_UNIFORM 176
MSI_IRQ_UNIFORM 177
MSI_IRQ_UNIFORM 178
MSI_IRQ_UNIFORM 179
MSI_IRQ_UNIFORM 180
MSI_IRQ_UNIFORM 181
MSI_IRQ_UNIFORM 182
MSI_IRQ_UNIFORM 183
MSI_IRQ_UNIFORM 184
MSI_IRQ_UNIFORM 185
MSI_IRQ_UNIFORM 186
MSI_IRQ_UNIFORM 187
MSI_IRQ_UNIFORM 188
MSI_IRQ_UNIFORM 189
MSI_IRQ_UNIFORM 190
MSI_IRQ_UNIFORM 191
MSI_IRQ_UNIFORM 192
MSI_IRQ_UNIFORM 193
MSI_IRQ_UNIFORM 194
MSI_IRQ_UNIFORM 195
MSI_IRQ_UNIFORM 196
MSI_IRQ_UNIFORM 197
MSI_IRQ_UNIFORM 198
MSI_IRQ_UNIFORM 199
MSI_IRQ_UNIFORM 200
MSI_IRQ_UNIFORM 201
MSI_IRQ_UNIFORM 202
MSI_IRQ_UNIFORM 203
MSI_IRQ_UNIFORM 204
MSI_IRQ_UNIFORM 205
MSI_IRQ_UNIFORM 206
MSI_IRQ_UNIFORM 207
MSI_IRQ_UNIFORM 208
MSI_IRQ_UNIFORM 209
MSI_IRQ_UNIFORM 210
MSI_IRQ_UNIFORM 211
MSI_IRQ_UNIFORM 212
MSI_IRQ_UNIFORM 213
MSI_IRQ_UNIFORM 214
MSI_IRQ_UNIFORM 215
MSI_IRQ_UNIFORM 216
MSI_IRQ_UNIFORM 217
MSI_IRQ_UNIFORM 218
MSI_IRQ_UNIFORM 219
MSI_IRQ_UNIFORM 220
MSI_IRQ_UNIFORM 221
MSI_IRQ_UNIFORM 222
MSI_IRQ_UNIFORM 223
MSI_IRQ_UNIFORM 224
MSI_IRQ_UNIFORM 225
MSI_IRQ_UNIFORM 226
MSI_IRQ_UNIFORM 227
MSI_IRQ_UNIFORM 228
MSI_IRQ_UNIFORM 229
MSI_IRQ_UNIFORM 230
MSI_IRQ_UNIFORM 231
MSI_IRQ_UNIFORM 232
MSI_IRQ_UNIFORM 233
MSI_IRQ_UNIFORM 234
MSI_IRQ_UNIFORM 235
MSI_IRQ_UNIFORM 236
MSI_IRQ_UNIFORM 237
MSI_IRQ_UNIFORM 238
MSI_IRQ_UNIFORM 239
MSI_IRQ_UNIFORM 240
MSI_IRQ_UNIFORM 241
MSI_IRQ_UNIFORM 242
MSI_IRQ_UNIFORM 243
MSI_IRQ_UNIFORM 244
MSI_IRQ_UNIFORM 245
MSI_IRQ_UNIFORM 246
MSI_IRQ_UNIFORM 247
MSI_IRQ_UNIFORM 248
MSI_IRQ_UNIFORM 249
MSI_IRQ_UNIFORM 250
MSI_IRQ_UNIFORM 251
MSI_IRQ_UNIFORM 252
MSI_IRQ_UNIFORM 253
MSI_IRQ_UNIFORM 254
MSI_IRQ_UNIFORM 255

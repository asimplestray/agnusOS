section .multiboot_header
header_start:
    dd 0xe85250d6                ; Magic number (multiboot2)
    dd 0                         ; Architecture 0 (protected mode i386)
    dd header_end - header_start ; Header length
    ; Checksum
    dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start))

    ; --- Framebuffer Request Tag ---
    dw 5                         ; tag type: framebuffer
    dw 0                         ; flags (0 = required)
    dd 20                        ; size of this tag (bytes)
    dd 1280                      ; preferred width
    dd 800                       ; preferred height
    dd 32                        ; preferred bits per pixel (32bpp ARGB)
    dd 0                         ; padding

    ; End tag (type 0, size 8)
    dw 0
    dw 0
    dd 8
header_end:

global multiboot_magic
global multiboot_info

section .data
align 4
multiboot_magic: dd 0
align 8
multiboot_info:  dq 0

section .text
bits 32
global start

start:
    ; Disable interrupts
    cli

    ; Save multiboot magic and info pointer (from eax and ebx)
    mov [multiboot_magic], eax
    mov [multiboot_info], ebx

    ; Initialize stack pointer
    mov esp, stack_top

    ; Run integrity checks
    call check_multiboot
    call check_cpuid
    call check_long_mode

    ; Set up paging structures
    call setup_page_tables

    ; Enable paging and transition to 64-bit Long Mode
    call enable_paging

    ; Load 64-bit GDT
    lgdt [gdt64.pointer]

    ; Far jump to 64-bit mode using retf
    push gdt64.code
    push start64
    retf

; Error routine: prints ERR: [Code] in red to VGA text memory
error:
    mov dword [0xb8000], 0x4f524f45 ; "ER" (Red on Black)
    mov dword [0xb8004], 0x4f3a4f52 ; "R:"
    mov dword [0xb8008], 0x4f204f20 ; "  "
    mov byte  [0xb800a], al          ; The character code
    hlt

check_multiboot:
    cmp eax, 0x36d76289
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, "M"                      ; Error code M = Multiboot failed
    jmp error

check_cpuid:
    ; Attempt to flip the ID bit (bit 21) in EFLAGS
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov al, "C"                      ; Error code C = CPUID not supported
    jmp error

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode
    ret
.no_long_mode:
    mov al, "L"                      ; Error code L = Long mode not supported
    jmp error

setup_page_tables:
    ; ── PML4[0] → PDPT ─────────────────────────────────────────────────────
    mov eax, page_table_l3
    or eax, 0b11
    mov [page_table_l4], eax

    ; ── PDPT[0..3] → PD0..PD3 (covers full 4 GB) ───────────────────────────
    mov eax, page_table_l2_0
    or eax, 0b11
    mov [page_table_l3 + 0*8], eax

    mov eax, page_table_l2_1
    or eax, 0b11
    mov [page_table_l3 + 1*8], eax

    mov eax, page_table_l2_2
    or eax, 0b11
    mov [page_table_l3 + 2*8], eax

    mov eax, page_table_l2_3
    or eax, 0b11
    mov [page_table_l3 + 3*8], eax

    ; ── PD0: identity map 0 – 1 GB ──────────────────────────────────────────
    mov ecx, 0
.map_pd0:
    mov eax, 0x200000                ; 2 MB per entry
    mul ecx                          ; eax = i * 2MB  (edx unused: < 2^32)
    or eax, 0b10000011               ; present + writable + huge
    mov [page_table_l2_0 + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd0

    ; ── PD1: identity map 1 – 2 GB (base = 0x40000000) ─────────────────────
    mov ecx, 0
.map_pd1:
    mov eax, 0x200000
    mul ecx
    add eax, 0x40000000              ; 1 GB offset
    or eax, 0b10000011
    mov [page_table_l2_1 + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd1

    ; ── PD2: identity map 2 – 3 GB (base = 0x80000000) ─────────────────────
    mov ecx, 0
.map_pd2:
    mov eax, 0x200000
    mul ecx
    add eax, 0x80000000              ; 2 GB offset
    or eax, 0b10000011
    mov [page_table_l2_2 + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd2

    ; ── PD3: identity map 3 – 4 GB (base = 0xC0000000) ─────────────────────
    ; Framebuffer is often at 0xE0000000 – 0xFD000000, covered here
    mov ecx, 0
.map_pd3:
    mov eax, 0x200000
    mul ecx
    add eax, 0xC0000000              ; 3 GB offset
    or eax, 0b10000011
    mov [page_table_l2_3 + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd3

    ret

enable_paging:
    ; Load PML4 address into cr3
    mov eax, page_table_l4
    mov cr3, eax

    ; Enable PAE (Physical Address Extension) in cr4
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Set LME (Long Mode Enable) in EFER MSR
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable Paging in cr0
    mov eax, cr0
    or eax, 1 << 31 | 1 << 0
    mov cr0, eax
    ret

section .rodata
gdt64:
    dq 0                             ; Null descriptor
.code: equ $ - gdt64
    ; 64-bit code segment: flag bits 44 (descriptor type), 47 (present), 53 (64-bit)
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)
.data: equ $ - gdt64
    ; 64-bit data segment: flag bits 41 (writable), 44 (descriptor type), 47 (present)
    dq (1 << 41) | (1 << 44) | (1 << 47)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .bss
align 4096
page_table_l4:
    resb 4096
page_table_l3:
    resb 4096
page_table_l2_0:                     ; 0   – 1 GB
    resb 4096
page_table_l2_1:                     ; 1   – 2 GB
    resb 4096
page_table_l2_2:                     ; 2   – 3 GB
    resb 4096
page_table_l2_3:                     ; 3   – 4 GB (framebuffer lives here!)
    resb 4096
stack_bottom:
    resb 4096 * 4                    ; 16 KB stack
stack_top:

section .text
bits 64
extern kernel_main
start64:
    ; Set segment registers
    mov ax, gdt64.data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Early debug: write "K" to VGA text mode
    mov word [0xb8000], 0x0f4b

    ; Early debug: write "E" to VGA text mode
    mov word [0xb8002], 0x0f45

    ; Jump to our C kernel
    call kernel_main

    ; Halt if kernel returns
    cli
.halt:
    hlt
    jmp .halt

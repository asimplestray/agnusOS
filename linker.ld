ENTRY(start)

SECTIONS
{
    /* Load kernel at 1MB physical address */
    . = 1M;

    _kernel_start = 1M;

    .text :
    {
        /* Ensure the multiboot header is placed at the very beginning */
        KEEP(*(.multiboot_header))
        *(.text)
    }

    .rodata :
    {
        *(.rodata)
    }

    .data :
    {
        *(.data)
    }

    .bss :
    {
        *(COMMON)
        *(.bss)
    }

    _kernel_end = .;
}

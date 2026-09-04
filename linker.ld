ENTRY(start)

PHDRS
{
    text PT_LOAD FLAGS(5);  /* R+X */
    data PT_LOAD FLAGS(6);  /* R+W */
}

SECTIONS
{
    /* Load kernel at 1MB physical address */
    . = 1M;

    _kernel_start = 1M;

    .text :
    {
        KEEP(*(.multiboot_header))
        *(.text)
    } :text

    .rodata :
    {
        *(.rodata)
        *(.rodata.*)
        *(.eh_frame)
    } :text

    .data :
    {
        *(.data)
        *(.data.*)
    } :data

    .bss :
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    } :data

    _kernel_end = .;
}

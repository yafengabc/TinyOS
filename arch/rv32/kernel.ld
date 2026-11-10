ENTRY(_start)
SECTIONS
{
    /* QEMU virt loads the ELF at 0x80000000. */
    . = 0x80000000;

    .text : {
        *(.text.entry)
        *(.text*)
    }

    .rodata : ALIGN(16) {
        *(.rodata*)
        *(.srodata*)
    }

    .data : ALIGN(16) {
        *(.data*)
        *(.sdata*)
    }

    /* Everything in here is NOBITS: it occupies RAM at run time but not
     * space in the ELF file.  start.S zeroes [__bss_start,__bss_end),
     * which deliberately covers the kernel stack (defined in start.S)
     * as well. */
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    __kernel_end = .;

    /* Task windows start at the first 64 KiB boundary after the kernel, so a
     * smaller heap or kernel stack hands the freed bytes to user windows. */
    __app_base = ALIGN(__kernel_end, 0x10000);
    ASSERT(__app_base + 0x10000 <= 0x81400000,
           "kernel leaves no room for a task window")

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
        *(.riscv.attributes)
    }
}

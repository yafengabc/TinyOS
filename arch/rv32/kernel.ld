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

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
        *(.riscv.attributes)
    }
}

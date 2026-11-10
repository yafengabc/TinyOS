ENTRY(_start)
SECTIONS
{
    . = 0x00010000;

    .text : {
        *(.text.entry)
        *(.text*)
    }

    .rodata : ALIGN(16) {
        *(.rodata*)
    }

    .data : ALIGN(16) {
        *(.data*)
    }

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    __kernel_end = .;

    /* Task windows start at the first 64 KiB boundary after the kernel, but
     * never below 1 MiB: 0xa0000-0x100000 is the VGA/BIOS hole, not RAM. */
    __app_base = ALIGN(MAX(__kernel_end, 0x00100000), 0x10000);
    ASSERT(__app_base + 0x10000 <= 0x08000000,
           "kernel leaves no room for a task window")

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
        *(.got*)
    }
}

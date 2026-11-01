/* TinyOS Cortex-M3 kernel linker script (QEMU mps2-an385).
 *
 * The board fetches the initial SP and the reset vector from the first two
 * words of the image, so the vector table must sit at 0x00000000.  Code
 * lives in "flash" at 0; .data/.bss/stacks live in SRAM at 0x20000000.
 *
 * Everything is placed at 0 (load address) and copied to RAM by _start,
 * which is what a real Cortex-M part does from its own flash on boot. */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

ENTRY(_start)

SECTIONS
{
    .text : {
        /* The vector table must be first: the core reads word 0 as the
           initial SP and word 1 as the reset vector. */
        KEEP(*(.vectors))
        *(.text.entry)
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* Copied from flash to RAM by _start. */
    .data : {
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM

    __kernel_end = .;

    /* The user windows start at PLAT_APP_BASE (board/qemu-mps2/layout.c).
       Nothing checks that at compile time, so catch a growing image here
       instead of letting the loader overwrite kernel .bss or the stack. */
    ASSERT(__kernel_end <= 0x20080000, "kernel image overflows the SRAM reserved for it")

    /DISCARD/ : {
        *(.comment)
        *(.ARM.exidx*)
        *(.ARM.attributes)
        *(.note*)
        *(.eh_frame*)
    }
}

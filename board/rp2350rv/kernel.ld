/* TinyOS RP2350 RISC-V kernel linker script.
 *
 * Hazard3 executes in place from the QSPI flash window at 0x10000000, just
 * like the ARM cores do.  Unlike ARM there is no vector table at flash
 * offset 0: the boot ROM scans the first 4 KiB for a PICOBIN IMAGE_DEF and
 * jumps wherever its ENTRY_POINT item says, so image_def.S's block is the
 * first word of the image and _start follows it.
 *
 * .text/.rodata stay in flash (XIP).  .data and .bss live in the 520 KiB of
 * SRAM at 0x20000000 and _start copies .data from its flash LMA - the ROM
 * loads no segments for a RISC-V flash image.  The littlefs partition stays
 * in flash at 0x80000 and is read through the XIP window.
 *
 * The first 128 KiB of SRAM are the kernel's (PLAT_KERNEL_MAX); the striped
 * banks above 0x20020000 are task windows, exactly like the ARM port. */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 16M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}

ENTRY(_start)

SECTIONS
{
    .text : {
        /* Must stay inside the first 4 KiB of flash -- the ROM only scans
           that window for the block loop.  KEEP defeats --gc-sections.
           This is the very first word of the image: on RISC-V the ROM does
           not need (and must not find) an ARM vector table here. */
        KEEP(*(.picobin_block))
        *(.text.entry)
        *(.text*)
        *(.rodata*)
        *(.srodata*)
        . = ALIGN(4);
    } > FLASH

    /* Copied from flash (LMA) to SRAM (VMA) by _start.  Functions placed in
       .data.ramfunc (the ROM flash writers) are copied here too: they must
       execute from SRAM while QSPI is in serial-command mode. */
    .data : {
        __data_start = .;
        *(.data*)
        *(.sdata*)
        . = ALIGN(16);
        __data_end = .;
    } > RAM AT > FLASH
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM

    __kernel_end = .;

    ASSERT(__image_def_end - ORIGIN(FLASH) <= 4096,
           "RP2350 RISC-V IMAGE_DEF fell outside the first 4 KiB of flash")
    ASSERT(__kernel_end <= 0x20020000,
           "kernel image overflows the SRAM reserved for it (PLAT_KERNEL_MAX)")

    /DISCARD/ : {
        *(.comment)
        *(.riscv.attributes)
        *(.note*)
        *(.eh_frame*)
    }
}

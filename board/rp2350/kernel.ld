/* TinyOS RP2350 kernel linker script.
 *
 * The RP2350 boot ROM scans the first 4 KiB of the QSPI flash (mapped XIP at
 * 0x10000000) for a PICOBIN IMAGE_DEF block, then takes the initial MSP and
 * reset vector from the ARM vector table at the very start of the image.
 * So, unlike the RP2040 port, there is no 256-byte stage-2 and no reserved
 * hole: .vectors is at 0x10000000 and image_def.S's 20-byte block follows
 * it.  (This is the same arrangement pico-sdk's crt0.S uses.)
 *
 * Code runs in place from XIP.  .data/.bss/stack live in the 520 KiB of
 * on-chip SRAM at 0x20000000; the user windows occupy the striped banks
 * up to 0x20080000 (the last 8 KiB are the scratch banks, left alone).
 *
 * There is no MMU.  The ARMv8-M MPU is deliberately left off (see
 * TOS_ARM_NO_MPU in arch/arm/platform.c), so user tasks run unprivileged
 * but see the flat address space, exactly like the RP2040 port. */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 16M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}

ENTRY(_start)

SECTIONS
{
    .text : {
        /* Word 0 = initial MSP, word 1 = reset vector.  The ROM reads
           these; ARMv8-M wants the table 128-byte aligned, and the flash
           origin already is. */
        KEEP(*(.vectors))
        /* Must stay inside the first 4 KiB of flash -- the ROM only scans
           that window for the block loop.  KEEP defeats --gc-sections. */
        KEEP(*(.picobin_block))
        *(.text.entry)
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* Copied from flash (LMA) to SRAM (VMA) by _start.  Functions placed in
       .data.ramfunc (the ROM flash writers) are copied here too: they must
       execute from SRAM while QSPI is in serial-command mode. */
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

    ASSERT(__image_def_end - ORIGIN(FLASH) <= 4096,
           "RP2350 IMAGE_DEF fell outside the first 4 KiB of flash")
    /* User windows start at the first 16 KiB boundary after the kernel. */
    __app_base = ALIGN(__kernel_end, 0x4000);
    ASSERT(__app_base + 0x4000 <= 0x20080000,
           "kernel leaves no room for a task window")

    /DISCARD/ : {
        *(.comment)
        *(.ARM.exidx*)
        *(.ARM.attributes)
        *(.note*)
        *(.eh_frame*)
    }
}

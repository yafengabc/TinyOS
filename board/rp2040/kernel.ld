/* TinyOS RP2040 kernel linker script.
 *
 * The RP2040 boots from external QSPI flash (XIP) at 0x10000000.  The boot
 * ROM loads a 256-byte stage-2 (board/rp2040/boot2.bin) from flash offset 0,
 * verifies a CRC-32/MPEG-2 over its first 252 bytes and jumps to 0x10000100.
 * That address is therefore where this kernel's vector table must live, so the
 * XIP FLASH region begins at 0x10000100 and the first 256 bytes of flash are
 * owned by the stage-2 (the UF2 packer drops it in front of the kernel).
 *
 * Code runs in place from XIP; .data/.bss/stack live in the 264 KiB of
 * on-chip SRAM at 0x20000000.  There is no MMU and no MPU, so the flat model
 * from the x86 port applies unchanged: a task window is just a region of SRAM. */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000100, LENGTH = 16M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 264K
}

ENTRY(_start)

SECTIONS
{
    .text : {
        /* The vector table must be first: the core reads word 0 as the
           initial MSP and word 1 as the reset vector (the stage-2 sets VTOR
           to 0x10000100 and branches to word 1). */
        KEEP(*(.vectors))
        *(.text.entry)
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* Copied from flash (LMA) to SRAM (VMA) by _start. */
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

    /* The kernel image + 32 KiB kernel stack must fit in the SRAM reserved
       for it before the user windows begin at PLAT_APP_BASE. */
    ASSERT(__kernel_end <= 0x20020000, "kernel image overflows the SRAM reserved for it (PLAT_KERNEL_MAX)")

    /DISCARD/ : {
        *(.comment)
        *(.ARM.exidx*)
        *(.ARM.attributes)
        *(.note*)
        *(.eh_frame*)
    }
}

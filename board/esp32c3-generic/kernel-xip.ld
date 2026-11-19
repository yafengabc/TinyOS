/* ESP32-C3 kernel linker script, XIP variant.
 *
 * Selected when the build target sets XipFlashBase (internal/build/build.go
 * looks for this file next to the board's kernel.ld).  Two regions:
 *
 *   0x40380000  SRAM  - entry stub, cache/MMU bring-up, trap machinery,
 *                       .data, .bss
 *   0x42000000  flash - .text and .rodata, executed in place through the
 *                       icache window
 *
 * The packer (internal/build/espimage.go) sees segments in the 0x42000000
 * window, keeps them out of the ROM-loadable image, and writes them at flash
 * offset XipFlashBase instead.  The boot stub in arch/rv32/start.S maps the
 * 64 KiB page at XipMapBase to the window; XipFlashBase may sit inside that
 * page (the 0x2000 variant), in which case the code VMA is
 * 0x42000000 + (XipFlashBase - XipMapBase).
 *
 * What must stay in SRAM, and why:
 *   .text.boot   runs before any mapping exists - it cannot call or load
 *                from flash, so no literal pool and no branches outside it.
 *   .text.trap   a trap can arrive while the flash is being erased or
 *                written, when fetching from it is impossible.
 */

ENTRY(_start)
SECTIONS
{
    . = 0x40380000;

    /* RISC-V code and data can end on a 2-byte boundary (compressed
       instructions, half-word objects), but the esp image packer copies
       whole words and rejects a short trailing partial word.  A bare
       `. = ALIGN(4)` only moves the location counter without emitting
       bytes, so Filesz stays short - align, then write an explicit
       padding word.  End of section = 4-aligned + 4 = always aligned. */
    .boot : {
        *(.text.boot)
        *(.text.entry)
        *(.text.trap*)
        . = ALIGN(4);
        LONG(0);
    }

    .data : ALIGN(16) {
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
        LONG(0);
    }

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    __kernel_end = .;

    /* The task area starts at the first 16 KiB boundary after the kernel, so
     * a smaller heap or kernel stack hands the freed bytes to user windows. */
    __app_base = ALIGN(__kernel_end, 0x4000);
    ASSERT(__app_base + 0x4000 <= 0x403DC000,
           "kernel leaves no room for a task window")

    /* The code half.  Its vaddr is the window base plus the code offset
     * inside the mapped 64 KiB page; the build passes it as
     * XIP_TEXT_VADDR (--defsym) and the packer indexes the blob from there. */
    .text XIP_TEXT_VADDR : {
        *(.text*)
        . = ALIGN(4);
        LONG(0);
    }

    .rodata : ALIGN(16) {
        *(.rodata*)
        *(.srodata*)
        . = ALIGN(4);
        LONG(0);
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
        *(.riscv.attributes)
    }
}

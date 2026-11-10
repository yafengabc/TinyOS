/* ESP32-C3 kernel linker script.
 *
 * The kernel is loaded into SRAM by the ROM from the ESP image at flash
 * offset 0.  Addresses use the instruction-bus window (0x4038...): the
 * real chip cannot fetch from the data alias, see docs/PLATFORM-DESIGN.md
 * §8.1.  The layout mirrors arch/rv32/kernel.ld; only the base differs.
 *
 * This is the all-SRAM layout.  board/esp32c3/kernel-xip.ld is the variant
 * that runs the code half in place out of flash, and is selected when the
 * target sets XipFlashBase (internal/build/build.go).
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
    .text : {
        *(.text.entry)
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

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
        *(.riscv.attributes)
    }
}

/* Payload starts right after the 256-byte boot2 that lives at 0x10000000. */
OUTPUT_FORMAT("elf32-littlearm")
ENTRY(reset)

SECTIONS
{
    . = 0x10000100;
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
    }
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.note*) }
}

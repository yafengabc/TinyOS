ENTRY(_start)
SECTIONS
{
    . = 0x7C00;
    .text : {
        *(.text.entry)
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
    }
}

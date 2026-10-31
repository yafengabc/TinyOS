/* The whole image in the instruction-bus range, one segment.  This is the
 * layout a TinyOS task window would use if the two aliases turned out to be
 * interchangeable: code, data, heap and stack all addressed from 0x4038_0000.
 * .data is folded into the .text output section so the result is a single
 * PT_LOAD and therefore a single ESP segment. */
ENTRY(_start)
SECTIONS
{
  . = 0x40380000;
  .text   : { *(.text) *(.text.*) *(.data) *(.data.*) *(.rodata) *(.rodata.*) }
  /DISCARD/ : { *(.eh_frame) *(.comment) *(.riscv.attributes) }
}

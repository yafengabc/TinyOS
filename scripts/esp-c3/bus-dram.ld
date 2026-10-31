/* Code at 0x4038_0000, marker at 0x3FC8_0000.  The ROM loads the data segment
 * first and the code second, and because the two addresses are the same
 * memory, the code overwrites the marker.  Whatever comes back is informative:
 * a byte means the data bus can read the 0x3FC8 range, a trap means it cannot,
 * and 0xb7 means it read the code that was loaded on top. */
ENTRY(_start)
SECTIONS
{
  . = 0x40380000;
  .text   : { *(.text) *(.text.*) }
  . = 0x3FC80000;
  .data   : { *(.data) *(.data.*) }
  /DISCARD/ : { *(.eh_frame) *(.comment) *(.riscv.attributes) }
}

/* Everything at the data-bus alias.  The ROM accepts the segment address and
 * prints the load and entry lines, but the CPU cannot fetch an instruction
 * from 0x3FC8_xxxx: it traps immediately and the ROM's handler shows up as a
 * Saved PC inside the ROM.  Kept as a probe because it is the one negative
 * result the whole window model rests on. */
ENTRY(_start)
SECTIONS
{
  . = 0x3FC80000;
  .text   : { *(.text) *(.text.*) *(.data) *(.data.*) *(.rodata) *(.rodata.*) }
  /DISCARD/ : { *(.eh_frame) *(.comment) *(.riscv.attributes) }
}

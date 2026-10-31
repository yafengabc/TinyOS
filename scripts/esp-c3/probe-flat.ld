/* Single segment in the instruction-bus range, everything folded into one
 * PT_LOAD.  Two segments work on the real chip but the ROM then prints an
 * extra load: line, which is exactly the noise that made the earlier probes
 * look like they had failed. */
ENTRY(_start)
SECTIONS
{
  . = 0x40380000;
  .text   : { *(.text) *(.text.*) *(.rodata) *(.rodata.*) *(.data) *(.data.*) }
  /DISCARD/ : { *(.eh_frame) *(.comment) *(.riscv.attributes) }
}

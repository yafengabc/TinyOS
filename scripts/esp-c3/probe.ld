/* One segment, at the address the experiment is about.  BASE comes from
 * the build script so the same source covers both variants. */
ENTRY(_start)
SECTIONS
{
  . = BASE;
  .text   : { *(.text) *(.text.*) }
  .rodata : { *(.rodata) *(.rodata.*) }
  .data   : { *(.data) *(.data.*) }
  .bss    : { *(.bss) *(.bss.*) *(COMMON) }
  /DISCARD/ : { *(.eh_frame) *(.comment) *(.riscv.attributes) }
}

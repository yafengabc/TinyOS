/* The probe runs from SRAM exactly like the kernel does: code at 0x4038_0000,
 * one PT_LOAD so the ROM gets a single segment to copy.  Nothing here is
 * mapped through the cache - that is what the probe is trying to turn on. */
ENTRY(_start)
SECTIONS
{
  . = 0x40380000;
  .text : { *(.text) *(.text.*) *(.rodata) *(.rodata.*) }
  /DISCARD/ : { *(.eh_frame) *(.comment) *(.riscv.attributes)
                *(.data) *(.data.*) *(.bss) *(.bss.*) }
}

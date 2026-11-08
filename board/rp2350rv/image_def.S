/*
 * RP2350 PICOBIN IMAGE_DEF for a RISC-V flash image.
 *
 * There is no ARM vector table to read MSP/PC from, so the ROM needs both
 * an IMAGE_TYPE that selects Hazard3 and an ENTRY_POINT item that says where
 * to jump and which stack to install.  This block must be the first thing
 * in flash and inside the first 4 KiB; the linker script keeps it there.
 *
 * Block layout (little endian):
 *
 *   0xffffded3                     PICOBIN_BLOCK_MARKER_START
 *   42 01 01 11                   IMAGE_TYPE item, 1 word, value 0x1101
 *                                 = EXE | CPU_RISCV | CHIP_RP2350
 *   44 03 00 00 <entry> <sp>      ENTRY_POINT item, 3 words
 *   ff 04 00 00                   LAST item, 4 preceding item words
 *   <next block offset>            patched by internal/build/uf2.go
 *   0xab123579                     PICOBIN_BLOCK_MARKER_END
 *
 * The image type deliberately has no security field: pico-sdk emits the
 * same 0x1101 for __riscv, and the ROM does not expect one for this CPU.
 */

    .section .picobin_block, "a"
    .balign 4
    .globl __image_def_start
__image_def_start:
    .word 0xffffded3

    /* IMAGE_TYPE: EXE(1) | CPU_RISCV(0x100) | CHIP_RP2350(0x1000). */
    .byte 0x42
    .byte 0x1
    .hword 0x1101

    /* ENTRY_POINT: entry address then initial stack pointer. */
    .byte 0x44
    .byte 0x3
    .byte 0
    .byte 0
    .word _start
    .word __kernel_stack_top

    /* LAST: size of the preceding item words, in words. */
    .byte 0xff
    .hword (__image_def_end - __image_def_start - 16) / 4
    .byte 0
    .word 0                     /* next block offset, patched by the packer */
    .word 0xab123579

    .globl __image_def_end
__image_def_end:

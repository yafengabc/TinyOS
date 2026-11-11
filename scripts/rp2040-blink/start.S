/* Minimal Cortex-M0+ vector table + entry for the RP2040 bring-up blinker.
 *
 * The payload is linked at 0x10000100, which is where the RP2040 boot2 looks
 * for the vector table (SP at +0x00, reset at +0x04).  `reset` is a
 * .thumb_func, so the symbol already has bit 0 set: writing `.word reset + 1`
 * here yields an even address and the CPU faults on the first instruction. */

    .syntax unified
    .thumb

    .section .vectors, "ax"
    .globl vectors
vectors:
    .word 0x20042000            /* top of SRAM                            */
    .word reset
    .rept 14
    .word hang + 1
    .endr

    .section .text
    .thumb_func
    .globl reset
reset:
    bl main
hang:
    b hang

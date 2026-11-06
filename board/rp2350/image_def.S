/*
 * RP2350 PICOBIN IMAGE_DEF - the minimum viable image metadata.
 *
 * RP2040 needed a 256-byte stage-2 with a CRC; RP2350 instead scans the
 * first 4 KiB of flash for a "block loop" and wants the ARM vector table at
 * the very start of the image (flash 0x10000000).  Our linker script puts
 * .vectors first and this 20-byte block immediately after it, which is the
 * same layout pico-sdk's crt0.S produces.
 *
 * Block layout (little endian):
 *
 *   0xffffded3                     PICOBIN_BLOCK_MARKER_START
 *   42 01 21 10                   IMAGE_TYPE item, 1 word, value 0x1021
 *                                = EXE | SECURITY_S | CPU_ARM | CHIP_RP2350
 *   ff 01 00 00                   LAST item, one preceding item word
 *   ........                       next block offset (patched by the packer)
 *   0xab123579                     PICOBIN_BLOCK_MARKER_END
 *
 * No signature/hash is required unless secure boot is enabled in OTP, and
 * no ENTRY_POINT/VECTOR_TABLE items are needed: on ARM the ROM takes the
 * initial MSP and reset vector from the vector table at the start of flash.
 *
 * The LAST item's next-block offset starts at 0 here and is patched by
 * internal/build/uf2.go (rp2350CloseBlockLoop), which also appends an
 * ignored end block and points it back here.  pico-sdk's crt0.S builds the
 * same two-block loop by default; the offsets are link-time values, so the
 * packer is the natural place to compute them for our flat flash image.
 */

    .syntax unified
    .thumb

    .section .picobin_block, "a"
    .p2align 2
    .globl __image_def_start
__image_def_start:
    .word   0xffffded3
    .byte   0x42, 0x01              /* 1BS_IMAGE_TYPE, 1 word          */
    .hword  0x1021                  /* EXE | ARM | RP2350 | Secure     */
    .byte   0xff                    /* 2BS_LAST                        */
    .hword  0x0001                  /* one item word before LAST       */
    .byte   0x00
    .word   0x00000000              /* single-block loop               */
    .word   0xab123579
    .globl __image_def_end
__image_def_end:

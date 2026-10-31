/* Which bus can reach which address, on a real ESP32-C3.
 *
 * The same 256 KiB of SRAM answers at two addresses, 0x4038_0000 through the
 * instruction bus and 0x3FC8_0000 through the data bus, 0x700000 apart.  The
 * address a segment is *loaded* at says nothing about which bus can reach it
 * later, and that is what decides whether a task window can stay flat.  One
 * image per question, in build/exp-c3:
 *
 *   bus-code   everything at 0x40380000           prints 'I'
 *   bus-data   code at 0x40380000, data at 0x3FC80000
 *   bus-fetch  everything at 0x3FC80000           never prints
 *
 * Reading the results:
 *
 *   'I' from bus-code      the data bus reaches the instruction-bus range, so
 *                          one flat base covers code and data alike.
 *   'D' from bus-data      the data bus reaches the data-bus range too.
 *   bus-fetch silent       instructions cannot be fetched from 0x3FC8_xxxx.
 *
 * Two traps worth knowing about, both learned the hard way:
 *
 * 1. A byte written to the UART FIFO is not a byte sent.  The ROM prints up to
 *    70 characters immediately before the jump, and anything written in the
 *    next several milliseconds queues behind that text.  If the program then
 *    resets, the FIFO is cleared and the byte vanishes without a trace.  Every
 *    write below is therefore followed by a spin long enough to let it leave
 *    the wire, which is the only reason the printed characters survive.
 *
 * 2. Writes to 0x3FC8_xxxx and writes to 0x4038_xxxx land in the same memory.
 *    bus-data's marker is written first and then overwritten by the code that
 *    is loaded after it, so the probe prints the opcode byte of the first
 *    instruction (0xb7, 'lui') instead of the 'D' it stored -- which is itself
 *    the proof that the two aliases are one piece of silicon.
 */
    .section .text
    .globl _start
_start:
    li   t0, 0x60000000       /* UART0 FIFO */
    li   t1, 0x48             /* 'H': UART works at all */
    sw   t1, 0(t0)
    li   t2, 200000
2:
    addi t2, t2, -1
    bnez t2, 2b

    la   t1, marker           /* absolute address, resolved by the linker */
    lbu  t1, 0(t1)            /* the load under test */
    sw   t1, 0(t0)

    li   t2, 200000
3:
    addi t2, t2, -1
    bnez t2, 3b
    li   t1, 0x2e             /* '.': control came back from the load */
    sw   t1, 0(t0)
    li   t1, 10
    sw   t1, 0(t0)
1:
    j    1b

    .balign 4
    .section .data
    .globl marker
marker:
    .byte MK
    .byte 0, 0, 0             /* ESP segments are loaded as whole words; a
                                 .balign here trips R_RISCV_ALIGN against the
                                 compressed instruction right before it */

/* Print one character to UART0 on an ESP32-C3 and hang.
 *
 * The only thing that matters here is where the code lives.  Linked at
 * 0x40380000 it arrives through the instruction bus; at 0x3FC80000 through
 * the data bus.  Whether the second one can be fetched from decides the
 * whole user-program model, so each variant gets its own letter and its own
 * flash image.
 *
 * UART0 is at 0x60000000 on the C3 (qemu-virt puts it at 0x10000000), FIFO
 * register at offset 0, low byte = data.  The ROM has just set the port up
 * for its own boot messages, so writing the FIFO is enough - no clock, no
 * baud rate, no pin mux.
 *
 * CH comes from the build script: 0x41 'A' for IRAM, 0x42 'B' for DRAM.
 */
    .section .text
    .globl _start
_start:
    li   t0, 0x60000000
    li   t1, CH
    sw   t1, 0(t0)
    li   t1, 10
    sw   t1, 0(t0)
1:
    j    1b

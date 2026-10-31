/* What does the C3 actually keep in mtvec?
 *
 * Writing the trap address 0x4038_006c read back as 0x4038_0001: the low
 * twelve bits vanished and bit 0 came back set.  Either mtvec is WARL with a
 * much coarser granularity than the spec's four bytes, or something else is
 * going on with CSR 0x305 on this core.
 *
 * Probe: write four different values, report what stuck.  mepc is written too,
 * as a control -- it has the same four-byte alignment rule, so if mepc keeps
 * its low bits and mtvec does not, the difference is mtvec specifically.
 *
 *   mtvec <- 0x12345678   read back
 *   mtvec <- 0x4038006c   read back
 *   mtvec <- 0x00000000   read back
 *   mepc  <- 0x12345678   read back
 */
    .option norvc

    .section .text
    .globl _start
_start:
    la   sp, stack_scratch_end
    call wait_drain

    la   a0, banner
    li   a1, banner_end - banner
    call puts

    li   t0, 0x12345678
    csrw mtvec, t0
    csrr s2, mtvec
    la   a0, l1
    li   a1, l1_end - l1
    call puts
    mv   a0, s2
    call put_hex

    la   t0, fnord
    csrw mtvec, t0
    csrr s2, mtvec
    la   a0, l2
    li   a1, l2_end - l2
    call puts
    mv   a0, s2
    call put_hex

    li   t0, 0
    csrw mtvec, t0
    csrr s2, mtvec
    la   a0, l3
    li   a1, l3_end - l3
    call puts
    mv   a0, s2
    call put_hex

    li   t0, 0x12345678
    csrw mepc, t0
    csrr s2, mepc
    la   a0, l4
    li   a1, l4_end - l4
    call puts
    mv   a0, s2
    call put_hex

    /* Also report the value the assembler thinks fnord has, so the two can be
       compared on the same line. */
    la   a0, l5
    li   a1, l5_end - l5
    call puts
    la   a0, fnord
    call put_hex

1:  j    1b

    .balign 4
fnord:
    j    fnord

    .section .rodata
banner:
    .ascii "MV\r\n"
banner_end:
l1:
    .ascii "mtvec<=12345678 : "
l1_end:
l2:
    .ascii "mtvec<=fnord    : "
l2_end:
l3:
    .ascii "mtvec<=0        : "
l3_end:
l4:
    .ascii "mepc <=12345678 : "
l4_end:
l5:
    .ascii "fnord  address  : "
l5_end:
    .balign 4

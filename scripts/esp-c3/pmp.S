/* What does the C3 mask ROM leave behind in the PMP?
 *
 * The port partitions the address space with four PMP entries per task, so it
 * matters whether the ROM already configured one and whether any is locked
 * (cfg bit 7): a locked entry cannot be rewritten from M-mode, and one that
 * covers the SRAM would block the whole scheme.
 *
 * Read only.  Nothing here writes a CSR, so a wrong guess cannot fault.
 *
 * Output, one 8-digit hex word per line, fixed order:
 *
 *   pmpcfg0  pmpcfg1  pmpcfg2  pmpcfg3
 *   pmpaddr0 .. pmpaddr15
 *
 * A word of all zeroes cannot distinguish "entry is off" from "entry does not
 * exist"; umode.S tells the two apart by writing to entry 15 and reading back.
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

    csrr a0, pmpcfg0
    call put_hex
    csrr a0, pmpcfg1
    call put_hex
    csrr a0, pmpcfg2
    call put_hex
    csrr a0, pmpcfg3
    call put_hex

    csrr a0, pmpaddr0
    call put_hex
    csrr a0, pmpaddr1
    call put_hex
    csrr a0, pmpaddr2
    call put_hex
    csrr a0, pmpaddr3
    call put_hex
    csrr a0, pmpaddr4
    call put_hex
    csrr a0, pmpaddr5
    call put_hex
    csrr a0, pmpaddr6
    call put_hex
    csrr a0, pmpaddr7
    call put_hex
    csrr a0, pmpaddr8
    call put_hex
    csrr a0, pmpaddr9
    call put_hex
    csrr a0, pmpaddr10
    call put_hex
    csrr a0, pmpaddr11
    call put_hex
    csrr a0, pmpaddr12
    call put_hex
    csrr a0, pmpaddr13
    call put_hex
    csrr a0, pmpaddr14
    call put_hex
    csrr a0, pmpaddr15
    call put_hex

    la   a0, done
    li   a1, done_end - done
    call puts

    /* The timer group watchdog resets the chip after about a second, so this
       run repeats.  That is fine: a repeat proves the report completed. */
1:  j    1b

    .section .rodata
banner:
    .ascii "PMPCFG0..3 A0..15\r\n"
banner_end:
done:
    .ascii "END\r\n"
done_end:
    .balign 4

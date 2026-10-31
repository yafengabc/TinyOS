/* Can the C3 run user code in U-mode, and how many PMP entries exist?
 *
 * Two unknowns gate the port.  First, pmp.S reads back all zeroes, but a word
 * of zeroes means "entry is off" just as well as "entry does not exist" -- so
 * write a marker into pmpaddr15 and read it back.  Second, nothing has yet
 * proved that a U-mode context can execute at all on this chip: with no PMP
 * entry matching, the spec says U-mode access is *denied*, so the ROM leaving
 * the PMP empty means a naive jump to U-mode would fault on the first fetch.
 *
 * The probe therefore grants one NAPOT entry covering all 4 GiB with RWX,
 * installs its own trap vector, mrets into U-mode, writes one character from
 * there, then ecalls back.  Reaching the trap handler with mcause=8 (ecall
 * from U-mode) means M-mode, U-mode, PMP and mtvec all work.
 *
 * Report:
 *
 *   UMn      n = number of implemented PMP entries (text: "lt16" if 15 is dead)
 *   u        written from U-mode
 *   T<mcause>E<mepc>   trap taken
 *   OK       mcause was 8
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

    /* ---- how many PMP entries are actually implemented? ---- */
    li   t0, 0xa5a5a5a5
    csrw pmpaddr15, t0
    csrr t1, pmpaddr15
    bne  t1, t0, 1f
    la   a0, n16
    li   a1, n16_end - n16
    call puts
    j    2f
1:  la   a0, n_lt16
    li   a1, n_lt16_end - n_lt16
    call puts
2:
    /* Leave entry 15 the way we found it, and it was off, so the value in
       pmpaddr15 is inert either way. */

    /* ---- one entry, 1 GiB at 0x4000_0000, RWX, NAPOT ---- */
    /* "pmpaddr all ones means the whole address space" is a corner of the
       spec, and the first attempt using it never reached U-mode.  Encode the
       range explicitly instead: base 0x4000_0000, size 2^30, and
       pmpaddr = (base>>2) | (size/8 - 1) = 0x1000_0000 | 0x07ff_ffff.
       That covers code at 0x4038_0000 and UART0 at 0x6000_0000 alike. */
    li   t0, 0x17ffffff
    csrw pmpaddr0, t0
    li   a0, 'a'
    call putc
    li   t0, 0x1f              /* R|W|X, A=NAPOT, L=0 */
    csrw pmpcfg0, t0
    li   a0, 'c'
    call putc
    fence

    /* ---- our own trap vector ---- */
    la   t0, trap
    csrw mtvec, t0
    li   a0, 'v'
    call putc

    /* Self-test the vector before trusting it with the real thing: an ecall
       from M-mode (mcause=11) must land in the handler and return.  If the ROM
       refused the mtvec write, this is where it shows, not three steps later
       inside a U-mode fault that looks identical to a broken PMP setup. */
    ecall
    call wait_drain

    /* ---- drop to U-mode ---- */
    csrr t0, mstatus
    li   t1, ~(3 << 11)
    and  t0, t0, t1            /* MPP = 0 -> U-mode */
    ori  t0, t0, (1 << 7)      /* MPIE = 1 */
    csrw mstatus, t0
    la   t0, u_entry
    csrw mepc, t0
    mret

/* ---- runs with the privilege of a user process ---- */
u_entry:
    li   t0, 0x60000000
    li   t1, 'u'
    sw   t1, 0(t0)             /* the one thing only U-mode can prove */
    ecall                      /* back to M-mode, mcause = 8 */

    /* ecall returns here only if something is very wrong */
    la   a0, failed
    li   a1, failed_end - failed
    call puts
3:  j    3b

/* ---- M-mode trap handler ---- */
/* mtvec on this core is WARL: bits [7:2] always read back zero and MODE is
   fixed at 1 (vectored), so the base must be 256-byte aligned or the low bits
   are silently dropped.  A handler at 0x4038_006c became base 0x4038_0000,
   which is _start -- every ecall restarted the program.  Synchronous
   exceptions jump to the base itself in vectored mode, so aligning the label
   is the whole fix. */
    .balign 256
trap:
    csrr s0, mcause
    csrr s1, mepc

    li   t0, 11                /* ecall from M-mode: the self-test */
    bne  s0, t0, 6f
    li   a0, 'M'
    call putc
    addi s1, s1, 4             /* step over the ecall and go back */
    csrw mepc, s1
    mret

6:  li   a0, 'T'
    call putc
    mv   a0, s0
    call put_hex
    li   a0, 'E'
    call putc
    mv   a0, s1
    call put_hex
    li   t0, 8                 /* ecall from U-mode */
    bne  s0, t0, 4f
    la   a0, ok
    li   a1, ok_end - ok
    call puts
    j    5f
4:  la   a0, other
    li   a1, other_end - other
    call puts
5:  j    5b

    .section .rodata
banner:
    .ascii "UM"
banner_end:
n16:
    .ascii "16\r\n"
n16_end:
n_lt16:
    .ascii "lt16\r\n"
n_lt16_end:
failed:
    .ascii "\r\necall came back\r\n"
failed_end:
ok:
    .ascii "OK\r\n"
ok_end:
other:
    .ascii "UNEXPECTED\r\n"
other_end:
    .balign 4

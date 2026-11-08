/*
 * TinyOS RV32 entry point and trap vector.
 *
 * QEMU virt memory map (rv32):
 *   0x80000000  RAM (128 MiB by default)
 *   0x10000000  16550 UART0
 *   0x02000000  CLINT (mtime at +0xBFF8, mtimecmp at +0x4000)
 *
 * The kernel runs in machine mode; user programs run in U-mode and enter
 * the kernel through ecall.  There is no MMU, so U-mode is kept out of
 * kernel memory by PMP (which, in the flat model, grants the whole address
 * space: see plat_init in arch/rv32/platform.c).
 */

/* How many CPUs this build supports.  The build passes -DTOS_MAX_CPUS for an
 * SMP target (qemu-virt with -smp 2); everything else is 1 and none of the
 * multi-core code below is assembled. */
#ifndef TOS_MAX_CPUS
#define TOS_MAX_CPUS 1
#endif

/* Bytes of kernel stack per CPU.  Must match KSTACK_BYTES in kernel/cpu.h -
 * cpu_init() compares the reserved pool against the constant and panics on a
 * mismatch, because a mismatch does not fail to link: it moves the overflow
 * guard word into whatever happens to sit below the stack.
 *
 * 16 KiB, not 32: the ARM port has run on 16 KiB since it existed (RP2350 has
 * 128 KiB for the whole kernel), and the deepest kernel path here is a trap
 * frame plus a syscall, not a user recursion - `py` recurses on its own stack
 * in user mode.  A whole 16 KiB of .bss buys a task window on boards this
 * small. */
#define KSTACK_SHIFT 14                 /* 16 KiB */

    .section .text.entry, "ax"
    .globl _start
_start:
    /* Force MIE off.  Reset does mask it, but the ESP32-C3's ROM hands
       over with mstatus.MIE = 1 (seen on the real chip: ticks arrived
       during early kmain), so this is not redundant - it normalizes both
       boards to a known state.  MIE comes back in sched_idle().  Writing
       the mie CSR itself would be the obvious belt-and-braces, but the
       ESP32-C3 has no mie CSR at all - the esp-riscv core routes every
       peripheral line through INTMTX and gates it with mstatus alone,
       and touching 0x304 is an illegal instruction (verified on the real
       chip, scripts/esp-c3/csr.S). */
    csrci   mstatus, 8

    /* Which hart are we?  Every hart starts here at reset, so the boot hart
       has to be singled out before it clears .bss - a second hart doing that
       concurrently would wipe memory the first one is already using. */
    csrr    t0, mhartid
    bnez    t0, .Lsecondary

    /* Park a secondary hart.  QEMU virt passes the hart id in a0 (0 on
       the one that matters); the C3's ROM hands over a0 = 17 - it is not
       a hart id at all - so this must not be the only thing deciding
       whether to continue.  mhartid is the real answer and is 0 on every
       single-hart board here. */

    .option push
    .option norelax
    la      sp, __kernel_stack_top
    .option pop

    /* Clear .bss (covers the kernel stack: it is .bss space). */
    .option push
    .option norelax
    la      t0, __bss_start
    la      t1, __bss_end
    .option pop
1:
    bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:
#ifdef TOS_COPY_DATA
    /* Loaded-image boards (RP2350 RISC-V) are started by a ROM that only
       jumps to the entry point: .text runs in place from flash, but .data
       has to be moved from its flash LMA to its SRAM VMA here, before any
       global is read.  The RP2350 XIP window is plain memory, so this is a
       word copy from one address to another.  Both ends are 4-byte aligned
       and the section is padded to a word by the board's linker script. */
    .option push
    .option norelax
    la      t0, __data_load
    la      t1, __data_start
    la      t2, __data_end
    .option pop
3:
    bgeu    t1, t2, 4f
    lw      t3, 0(t0)
    sw      t3, 0(t1)
    addi    t0, t0, 4
    addi    t1, t1, 4
    j       3b
4:
#endif

    .option push
    .option norelax
    la      t0, trap_vectors
    .option pop
    csrw    mtvec, t0

#ifdef TOS_XIP_FLASH_BASE
    /* Map the IROM window onto the code half of the image.
     *
     * 0x42000000 is pure translation on this chip: one 4-byte MMU entry per
     * 64 KiB page at 0x600C5000, entry = physical address / 64 KiB, 0x100 =
     * unmapped.  The ROM's own helpers (Cache_Ibus_MMU_Set,
     * spi_flash_enable_cache) report success without changing anything here,
     * so the table is written directly.  All of it checked on hardware,
     * scripts/esp-c3/irom.S.
     *
     * This runs from RAM, before the window resolves, so it uses no literal
     * pool, no data and no calls into flash. */

    /* 1. cache owner/mode/enable.  The table alone is not enough: the
     *    instruction path stays shut until this has run. */
    li      t0, 0x400004c8          /* ROM_Boot_Cache_Init */
    jalr    ra, t0, 0

    /* 2. the two gate bits.  ICACHE_ENABLE resets to 0 and
     *    ICACHE_SHUT_IBUS/DBUS reset to 1, which is why a mapping written
     *    without them leaves the first fetch hanging forever. */
    li      t0, 0x600C4000
    lw      t1, 0(t0)
    ori     t1, t1, 1
    sw      t1, 0(t0)

    li      t0, 0x600C4004
    lw      t1, 0(t0)
    andi    t1, t1, -4              /* clear SHUT_IBUS | SHUT_DBUS */
    sw      t1, 0(t0)

    /* 3. the table, then drop whatever the cache picked up under the ROM's
     *    mapping. */
    call    xip_map_window
    li      t0, 0x400004d8          /* Cache_Invalidate_ICache_All */
    jalr    ra, t0, 0

    /* 4. hand the trap vector to the real table before anything can trap.
     *    This chip runs mtvec in vectored mode, so a stale base sends an
     *    interrupt to base + 4*code instead of the handler. */
    .option push
    .option norelax
    la      t0, trap_vectors
    .option pop
    csrw    mtvec, t0
#endif  /* TOS_XIP_FLASH_BASE */

    /* Into the kernel.  Everything from here on executes out of flash. */
    call    kmain

park:
    wfi
    j       park

/* -------------------------------------------------------------------- */
/* Secondary harts: wait for the boot CPU to say the kernel is usable,   */
/* then join the scheduler.  They must not touch .bss or any kernel      */
/* structure before that - the boot CPU is still clearing .bss.          */

.Lsecondary:
    la      t0, cpu_release
    li      t1, 0x534D5031              /* "SMP1", see kernel/cpu.c */
1:  lw      t2, 0(t0)
    bne     t2, t1, 1b

    .option push
    .option norelax
    la      sp, __kernel_stack_top
    .option pop
    csrr    t0, mhartid
    slli    t0, t0, KSTACK_SHIFT
    sub     sp, sp, t0                  /* this hart's own kernel stack */

#if TOS_MAX_CPUS > 1
    .option push
    .option norelax
    la      t0, trap_vectors_h1
    .option pop
    csrw    mtvec, t0
#endif

    call    cpu_secondary_main          /* never returns */
    j       park

/* -------------------------------------------------------------------- */
/* Trap frame layout shared with C (arch/rv32/regs.h):                  */
/*     0..124   x0..x31 (offset == 4 * register number)                 */
/*   128        mepc                                                    */
/*   132        mstatus                                                 */
/*   136        mcause                                                  */
/*   140        mtval                                                   */
/*   = 144 bytes, 16-byte aligned                                       */
/*                                                                      */
/* Two stacks meet here.  A trap from U-mode switches to the kernel      */
/* stack before pushing the frame; a trap from M-mode (timer interrupt   */
/* while a syscall runs, or a kernel bug) pushes onto whichever kernel   */
/* stack is already in use, which is what makes nesting safe.  A trap    */
/* clears mstatus.MIE in hardware, so the entry sequence itself cannot   */
/* be interrupted and may use t0 as scratch.                            */

    .equ FRAME_SIZE, 144

    .section .text.trap, "ax"
    .align 4
    .globl trap_entry

/* Vectored trap table.  The ESP32-C3 hard-wires mtvec to vectored mode
 * (verified on the real chip: writing 0x403800c0 reads back 0x40380001)
 * and its base must be 256-byte aligned - anything below that gets
 * zeroed, which silently redirects every synchronous exception to the
 * 256-byte boundary.  QEMU virt accepts vectored mode as well, so both
 * boards share this table: cause 0 (all synchronous exceptions) enters
 * through the base, interrupts enter at base + 4*cause.  Every slot
 * jumps to the same handler because the C side dispatches on mcause.
 * Entries are forced to 4 bytes with norvc: the assembler would shrink
 * `j` to 2-byte c.j, and base + 4*cause then lands mid-table (seen on
 * the real C3: an arriving tick was re-routed into the sync handler). */
    .option push
    .option norvc
    .balign 256
    .globl trap_vectors
trap_vectors:
    j       trap_entry_h0           /* 0: synchronous exceptions land here */
    .rept 31
    j       trap_entry_h0
    .endr
    .option pop

#if TOS_MAX_CPUS > 1
/* A second hart needs its own 256-byte table: mtvec is a per-hart CSR, and
 * the entry has to know which kernel stack to switch to.  The two tables are
 * emitted back to back so one NAPOT PMP entry covers both. */
    .option push
    .option norvc
    .balign 256
    .globl trap_vectors_h1
trap_vectors_h1:
    j       trap_entry_h1
    .rept 31
    j       trap_entry_h1
    .endr
    .option pop
#endif

/* One trap entry per hart.
 *
 * `kstack` is the top of that hart's kernel stack, an absolute expression the
 * linker folds into the la - which is what lets the entry switch stacks with
 * a single scratch register.  The sequence is: park t0 in mscratch (the only
 * place a trap may write before it has a stack), load the stack top, stash
 * the interrupted sp just above the frame, then recover t0 from mscratch. */
    .macro TRAP_ENTRY name, kstack
\name:
    csrw    mscratch, t0            /* park t0 */

    csrr    t0, mstatus
    srli    t0, t0, 11
    andi    t0, t0, 3               /* mstatus.MPP */
    beqz    t0, .Luser_\name        /* 0 = came from U-mode */

    /* ---- trap from M-mode: the current stack is already a kernel one --- */
    addi    sp, sp, -FRAME_SIZE
    csrr    t0, mscratch            /* original t0 */
    sw      t0, 20(sp)              /* frame->x5 */
    addi    t0, sp, FRAME_SIZE
    sw      t0, 8(sp)               /* frame->sp = sp at trap */
    j       .Lsave

.Luser_\name:
    /* ---- trap from U-mode: switch to this hart's kernel stack --------- */
    .option push
    .option norelax
    la      t0, \kstack
    .option pop
    sw      sp, -4(t0)              /* park the interrupted sp */
    addi    sp, t0, -(FRAME_SIZE + 16)
    lw      t0, (FRAME_SIZE + 12)(sp)
    sw      t0, 8(sp)               /* frame->sp = user sp */
    csrr    t0, mscratch            /* original t0 */
    sw      t0, 20(sp)              /* frame->x5 */
    j       .Lsave
    .endm

    TRAP_ENTRY trap_entry_h0, __kernel_stack_top
#if TOS_MAX_CPUS > 1
    TRAP_ENTRY trap_entry_h1, __kernel_stack_top - (1 << KSTACK_SHIFT)
#endif

.Lsave:
    sw      x1,    4(sp)
    sw      x3,   12(sp)            /* gp (unused, but keep the frame whole) */
    sw      x4,   16(sp)            /* tp */
    sw      x6,   24(sp)
    sw      x7,   28(sp)
    sw      x8,   32(sp)
    sw      x9,   36(sp)
    sw      x10,  40(sp)
    sw      x11,  44(sp)
    sw      x12,  48(sp)
    sw      x13,  52(sp)
    sw      x14,  56(sp)
    sw      x15,  60(sp)
    sw      x16,  64(sp)
    sw      x17,  68(sp)
    sw      x18,  72(sp)
    sw      x19,  76(sp)
    sw      x20,  80(sp)
    sw      x21,  84(sp)
    sw      x22,  88(sp)
    sw      x23,  92(sp)
    sw      x24,  96(sp)
    sw      x25, 100(sp)
    sw      x26, 104(sp)
    sw      x27, 108(sp)
    sw      x28, 112(sp)
    sw      x29, 116(sp)
    sw      x30, 120(sp)
    sw      x31, 124(sp)

    csrr    t0, mepc
    sw      t0, 128(sp)
    csrr    t0, mstatus
    sw      t0, 132(sp)
    csrr    t0, mcause
    sw      t0, 136(sp)
    csrr    t0, mtval
    sw      t0, 140(sp)

    mv      a0, sp
    call    trap_handler

    /* trap_handler returns the frame to resume on (exec and exit can
       replace it with a parent's saved frame). */
    mv      sp, a0
    j       .Lrestore

/* -------------------------------------------------------------------- */
/* Resume a frame: restore CSRs and registers, then mret.  a0 hands the  */
/* frame in (sp follows it).  Shared with plat_enter_user(), which is    */
/* how the very first user program is started.                          */

    .globl rv32_enter_user
rv32_enter_user:
    mv      sp, a0
    /* fall through */

.Lrestore:
    lw      t0, 128(sp)
    csrw    mepc, t0
    lw      t0, 132(sp)
    csrw    mstatus, t0

    /* Where does sp point after this?  Returning to U-mode restores the
       user stack the trap interrupted (frame->sp); returning to M-mode
       simply unwinds the frame. */
    srli    t1, t0, 11
    andi    t1, t1, 3
    bnez    t1, .Lkernel_sp
    j       .Lrestore_regs
.Lkernel_sp:
    addi    t0, sp, FRAME_SIZE
    sw      t0, 8(sp)

.Lrestore_regs:
    lw      x1,    4(sp)
    lw      x3,   12(sp)
    lw      x4,   16(sp)
    lw      x6,   24(sp)
    lw      x7,   28(sp)
    lw      x8,   32(sp)
    lw      x9,   36(sp)
    lw      x10,  40(sp)
    lw      x11,  44(sp)
    lw      x12,  48(sp)
    lw      x13,  52(sp)
    lw      x14,  56(sp)
    lw      x15,  60(sp)
    lw      x16,  64(sp)
    lw      x17,  68(sp)
    lw      x18,  72(sp)
    lw      x19,  76(sp)
    lw      x20,  80(sp)
    lw      x21,  84(sp)
    lw      x22,  88(sp)
    lw      x23,  92(sp)
    lw      x24,  96(sp)
    lw      x25, 100(sp)
    lw      x26, 104(sp)
    lw      x27, 108(sp)
    lw      x28, 112(sp)
    lw      x29, 116(sp)
    lw      x30, 120(sp)
    lw      x31, 124(sp)
    lw      x5,   20(sp)
    lw      sp,    8(sp)
    mret

/* -------------------------------------------------------------------- */

    .section .bss
    .balign 16
    .globl __kernel_stack
__kernel_stack:
    /* One stack per CPU, allocated statically: hart N gets
     * [__kernel_stack_top - (N+1)*KSTACK_SHIFT, __kernel_stack_top - N*KSTACK_SHIFT).
     * Sized from the same macro the trap entry uses, so the two cannot
     * drift apart the way they once did on i386. */
    .space (1 << KSTACK_SHIFT) * TOS_MAX_CPUS
    .globl __kernel_stack_top
__kernel_stack_top:

/* -------------------------------------------------------------------- */
/* XIP bring-up diagnostics.                                            */
/*                                                                      */
/* Lives in .text.boot, which the XIP linker script keeps in SRAM:      */
/* everything here has to work before the flash mapping exists, so it   */
/* uses no literal pool, calls nothing, and loads nothing.              */

#ifdef TOS_XIP_FLASH_BASE

    .section .text.boot, "ax"
    .align 4

/* xip_map_window(): point the whole code window at the image.
 *
 * One entry per 64 KiB page; the entry holds the physical address divided by
 * the page size, not the address itself - writing the byte address makes the
 * window address a page number far past the end of the chip, and the first
 * fetch through it never completes (measured). */
xip_map_window:
    li      t0, 0x600C5000
    li      t1, (TOS_XIP_FLASH_BASE >> 16)
    li      t2, 56                  /* 3.5 MiB of code from the XIP base */
    li      t3, 1
1:  sw      t1, 0(t0)
    addi    t0, t0, 4
    add     t1, t1, t3
    addi    t2, t2, -1
    bnez    t2, 1b
    ret

#endif  /* TOS_XIP_FLASH_BASE */

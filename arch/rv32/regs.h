#ifndef TINYOS_ARCH_REGS_H
#define TINYOS_ARCH_REGS_H

#include "types.h"

/* Register frame the RV32 port pushes in trap_entry (arch/rv32/start.S).
 *
 * Layout, in words: the 32 integer registers in numeric order (x0/zero is
 * only there to keep "offset == 4 * register number" true), then the four
 * CSRs the trap touched.  36 words = 144 bytes.
 *
 * `sp` holds the stack pointer the trap interrupted, which is *not* the
 * stack the frame itself lives on once user mode is involved: the trap
 * handler runs on the kernel stack and frame->sp is the user stack that
 * mret must restore. */
typedef struct regs {
    uint32_t zero;                     /* x0,  always 0                */
    uint32_t ra, sp, gp, tp;           /* x1..x4                       */
    uint32_t t0, t1, t2;               /* x5..x7                       */
    uint32_t s0, s1;                   /* x8..x9                       */
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;   /* x10..x17             */
    uint32_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11; /* x18..x27    */
    uint32_t t3, t4, t5, t6;           /* x28..x31                     */
    uint32_t mepc, mstatus, mcause, mtval;
} regs_t;

/* mcause values we care about.  The top bit marks an interrupt; the low
 * bits are the cause (qemu-virt CLINT timer = 7, machine external = 11)
 * or, on the ESP32-C3, the routed INTMTX CPU LINE (the tick arrives as
 * 0x80000000|2 - verified on the real chip). */
#define RV_MCAUSE_IRQ_M_TIMER 0x80000007u
#define RV_MCAUSE_IRQ_M_EXT   0x8000000bu
#define RV_MCAUSE_IRQ_BIT     0x80000000u
#define RV_MCAUSE_ECALL_U     8u
#define RV_MCAUSE_ILLEGAL     2u
#define RV_MCAUSE_LOAD_FAULT  5u
#define RV_MCAUSE_STORE_FAULT 7u

/* mstatus fields. */
#define RV_MSTATUS_MPP_MASK   0x00001800u
#define RV_MSTATUS_MPP_U      0x00000000u
#define RV_MSTATUS_MPP_M      0x00001800u
#define RV_MSTATUS_MPIE       0x00000080u
#define RV_MSTATUS_MIE        0x00000008u

#endif

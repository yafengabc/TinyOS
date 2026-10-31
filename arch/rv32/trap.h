#ifndef TINYOS_RV32_TRAP_H
#define TINYOS_RV32_TRAP_H

#include "types.h"

/* Frame pushed by trap_entry in arch/rv32/start.S. */
typedef struct {
    uint32_t ra, t0, t1, t2, a0, a1, a2, a3;
    uint32_t a4, a5, a6, a7, t3, t4, t5, t6;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7;
    uint32_t s8, s9, s10, s11;
    uint32_t mstatus, mepc, mcause, mtval;
} rv32_frame_t;

/* mcause values we care about */
#define RV_MCAUSE_IRQ_M_TIMER 0x80000007u

#endif

#ifndef TINYOS_ARCH_REGS_H
#define TINYOS_ARCH_REGS_H

#include "types.h"

/* Register frame for the Cortex-M port (arch/arm/start.S).
 *
 * Exception entry builds an 80-byte block on the kernel stack; the first
 * four words are bookkeeping, the next eight are the saved r4..r11 and the
 * last eight are a copy of the hardware frame the core pushed (r0,r1,r2,r3,
 * r12,lr,pc,xpsr).  arm_resume writes that copy back before returning, so
 * C code edits registers by writing these fields.
 *
 * The layout is shared with assembly, which uses byte offsets (FR_EXC,
 * FR_HW, FR_R0, FR_SIZE in start.S).  There must be no padding here: an
 * extra word shifts r0..xpsr by four bytes, and the frame view then reads
 * lr where pc belongs.  The size check below catches that at compile time.
 */
typedef struct regs {
    uint32_t r4, r5, r6, r7;      /* 0..3    saved callee-saved       */
    uint32_t r8, r9, r10, r11;    /* 4..7                             */
    uint32_t exc_return;          /* 8       as received at entry     */
    uint32_t vec;                 /* 9       exception number         */
    uint32_t hw;                  /* 10      hardware frame address   */
    uint32_t kern_sp;             /* 11      kernel sp on return      */

    uint32_t r0, r1, r2, r3;      /* 12..15  hardware r0..r3          */
    uint32_t r12;                 /* 16                               */
    uint32_t lr;                  /* 17                               */
    uint32_t pc;                  /* 18                               */
    uint32_t xpsr;                /* 19                               */
} regs_t;

/* FR_SIZE in start.S is 80 (4 bookkeeping + 8 saved + 8 hardware words). */
typedef char arm_regs_frame_size_check[(sizeof(regs_t) == 80) ? 1 : -1];

/* Hardware frame sizes and offsets (ARMv7-M stacking order). */
#define ARM_HW_SIZE   32
#define ARM_HW_R0     0
#define ARM_HW_R1     4
#define ARM_HW_R2     8
#define ARM_HW_R3     12
#define ARM_HW_R12    16
#define ARM_HW_LR     20
#define ARM_HW_PC     24
#define ARM_HW_XPSR   28

/* Cortex-M exception numbers (vector table indices). */
#define ARM_VEC_HARDFAULT   3
#define ARM_VEC_MEMMANAGE   4
#define ARM_VEC_BUSFAULT    5
#define ARM_VEC_USAGEFAULT  6
#define ARM_VEC_SVCALL      11
#define ARM_VEC_SYSTICK     15

/* EXC_RETURN values.  Bit 2 (S) is 1 when returning to Thread mode, which
 * is where user programs run. */
#define ARM_EXC_RET_THREAD     0xFFFFFFFDu
#define ARM_EXC_RET_HANDLER    0xFFFFFFF9u
#define ARM_EXC_RET_S_BIT      0x00000004u

/* Initial xPSR for a fresh user task: only the Thumb bit (24) is set. */
#define ARM_XPSR_THUMB         0x01000000u

#endif

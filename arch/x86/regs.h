#ifndef TINYOS_ARCH_REGS_H
#define TINYOS_ARCH_REGS_H

#include "types.h"

/* Register frame the i386 port pushes in isr.S (see arch/x86/isr.S for the
 * exact stack layout; it must stay in sync with this struct).
 *
 * Core code never reads the individual registers: it goes through
 * plat_frame_get_syscall() / plat_frame_set_ret() and friends, which keeps
 * kernel/ free of eax/ebx/eip. */
typedef struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t vector, err;
    uint32_t eip, cs, eflags, useresp, ss;
} regs_t;

#endif

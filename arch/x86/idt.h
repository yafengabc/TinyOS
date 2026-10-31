#ifndef TINYOS_IDT_H
#define TINYOS_IDT_H

#include "types.h"
#include "platform.h"   /* regs_t */

void idt_init(void);
void pic_remap(void);
void pic_eoi(int irq);
void pic_mask_all_except(uint32_t mask);

/* Trap handlers return the frame to resume: the scheduler (and blocking
 * syscalls) may hand back a different task's frame. */
regs_t *irq_dispatch(regs_t *r);
regs_t *exception_dispatch(regs_t *r);
regs_t *isr_dispatch(regs_t *r);

#endif

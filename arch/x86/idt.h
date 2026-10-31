#ifndef TINYOS_IDT_H
#define TINYOS_IDT_H

#include "types.h"
#include "platform.h"   /* regs_t */

void idt_init(void);
void pic_remap(void);
void pic_eoi(int irq);
void pic_mask_all_except(uint32_t mask);
void irq_dispatch(regs_t *r);
void syscall_dispatch(regs_t *r);
void exception_dispatch(regs_t *r);

#endif

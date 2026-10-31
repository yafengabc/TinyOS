#include "idt.h"
#include "gdt.h"
#include "console.h"
#include "io.h"
#include "timer.h"
#include "keyboard.h"
#include "process.h"

struct idt_entry {
    uint16_t off_low;
    uint16_t sel;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t off_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);
extern void isr32(void); extern void isr33(void); extern void isr34(void);
extern void isr35(void); extern void isr36(void); extern void isr37(void);
extern void isr38(void); extern void isr39(void); extern void isr40(void);
extern void isr41(void); extern void isr42(void); extern void isr43(void);
extern void isr44(void); extern void isr45(void); extern void isr46(void);
extern void isr47(void);
extern void isr128(void);
extern void isr_default(void);

static const char *exception_names[32] = {
    "divide by zero", "debug", "NMI", "breakpoint",
    "overflow", "bound range", "invalid opcode", "device not available",
    "double fault", "coprocessor segment", "invalid TSS", "segment not present",
    "stack fault", "general protection fault", "page fault", "reserved",
    "x87 floating point", "alignment check", "machine check", "SIMD floating point",
    "virtualization", "control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
    "hypervisor", "VMM communication", "security", "reserved"
};

static void idt_set_gate(int n, void (*handler)(void), uint8_t flags)
{
    uint32_t addr = (uint32_t)handler;
    idt[n].off_low  = (uint16_t)(addr & 0xFFFF);
    idt[n].off_high = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[n].sel      = KERNEL_CS;
    idt[n].zero     = 0;
    idt[n].flags    = flags;
}

void pic_remap(void)
{
    uint8_t mask1 = inb(0x21);
    uint8_t mask2 = inb(0xA1);

    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();
    outb(0x21, 0x20);
    io_wait();
    outb(0xA1, 0x28);
    io_wait();
    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();
    outb(0x21, 0x01);
    io_wait();
    outb(0xA1, 0x01);
    io_wait();

    outb(0x21, mask1);
    outb(0xA1, mask2);
}

void pic_eoi(int irq)
{
    if (irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void pic_mask_all_except(uint32_t mask)
{
    outb(0x21, (uint8_t)(~mask & 0xFF));
    outb(0xA1, 0xFF); /* mask everything on the slave PIC for now */
}

void idt_init(void)
{
    void (*handlers[48])(void) = {
        isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
        isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
        isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39,
        isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47
    };

    for (int i = 0; i < 256; i++)
        idt_set_gate(i, isr_default, 0x8E);

    for (int i = 0; i < 48; i++)
        idt_set_gate(i, handlers[i], 0x8E);

    /* system call gate, callable from ring 3 */
    idt_set_gate(0x80, isr128, 0xEE);

    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base  = (uint32_t)&idt;
    __asm__ volatile("lidt %0" : : "m"(idtp));
}

void exception_dispatch(regs_t *r)
{
    uint32_t cr2 = 0;
    if (r->vector == 14)
        cr2 = read_cr2();

    /* Fault inside a user program: kill it, keep the system alive. */
    if ((r->cs & 3) == 3) {
        kprintf("\n[pid %d] %s at eip=0x%08x%s\n",
                process_current_pid(), exception_names[r->vector & 31], r->eip,
                r->vector == 14 ? "" : "");
        if (r->vector == 14)
            kprintf("         faulting address 0x%08x\n", cr2);
        process_kill_current(r, 139);
        return;
    }

    cli();
    kprintf("\n*** KERNEL EXCEPTION: %s (%d) ***\n",
            exception_names[r->vector & 31], r->vector);
    kprintf("err=0x%08x eip=0x%08x cs=0x%08x eflags=0x%08x\n",
            r->err, r->eip, r->cs, r->eflags);
    if (r->vector == 14)
        kprintf("cr2=0x%08x\n", cr2);
    kprintf("System halted.\n");
    for (;;)
        hlt();
}

void isr_dispatch(regs_t *r)
{
    if (r->vector < 32) {
        exception_dispatch(r);
        return;
    }
    if (r->vector >= 32 && r->vector < 48) {
        irq_dispatch(r);
        pic_eoi((int)r->vector - 32);
        return;
    }
    if (r->vector == 0x80) {
        syscall_dispatch(r);
        return;
    }
    kprintf("\nUnexpected interrupt %d\n", r->vector);
}

void irq_dispatch(regs_t *r)
{
    switch (r->vector - 32) {
    case 0:
        timer_tick();
        break;
    case 1:
        keyboard_irq();
        break;
    case 7:
        /* spurious IRQ7, nothing to do */
        break;
    default:
        break;
    }
}

#ifndef TINYOS_PLAT_MEMPROT_H
#define TINYOS_PLAT_MEMPROT_H

#include "types.h"

/* ---- user mode ------------------------------------------------------- */

/* There is no address-space abstraction: the unified model is a single flat
 * address space, and the kernel reads and writes user memory directly.
 * Kernel code validates user pointers against the current task's window
 * (process_user_ok).
 *
 * Hardware may still be able to wall user mode off from the rest of the
 * machine, and where it can, it should: rv32 uses PMP to give each task
 * code / data / stack and nothing else.  x86-32 with paging disabled can
 * do nothing here (see docs/PLATFORM-DESIGN.md). */

/* One contiguous piece of the task that is about to run.  `attr` is the
 * kernel's intent, not a register encoding: how a port reaches it is the
 * port's business. */
enum {
    PLAT_SEG_CODE = 0,      /* image: user read + execute, no write */
    PLAT_SEG_DATA = 1,      /* data + stack: user read + write      */
};

typedef struct {
    uint32_t base;          /* inclusive */
    uint32_t top;           /* exclusive */
    uint32_t attr;          /* PLAT_SEG_* */
} plat_seg_t;

/* A task never has more than this: code, data+heap, stack. */
#define PLAT_SEG_MAX 3

/* Program the protection for the task that is about to run, then forget
 * about it: memory protection is a property of the running task, not of
 * the kernel.
 *
 * The caller sends the pieces, in ascending order, non-overlapping:
 *
 *   PLAT_SEG_CODE   [win_base, wx)          image, ends where .data starts
 *   PLAT_SEG_DATA   [wx, brk)               data + heap as sbrk() took it
 *   PLAT_SEG_DATA   [stack_low, stack_top)  stack, stack_top == win_end
 *
 * Any piece may be empty and is then simply absent; a gap between two pieces
 * is deliberate - it is the hole between the break and the stack that keeps a
 * heap running off its end and a stack running off its bottom from meeting.
 *
 * **A port must never cover a gap.**  Not because the bytes matter, but
 * because `prot heap` and `prot stack` are exactly that gap: a port that
 * rounds a region up to its granularity and spills into the hole turns a
 * fault into a silent write, and the two tests that would have caught it are
 * the ones it just disabled.
 *
 * Everything else - the kernel's own memory, MMIO, other tasks' windows - is
 * out of reach for user mode.  x86-32 with paging disabled can do nothing
 * here (see docs/PLATFORM-DESIGN.md).
 *
 * How closely a port can follow the bounds is its own business, and it must
 * say so in a comment where it degrades:
 *
 *   - TOR (rv32 without a board hook): exact, boundaries are the encoding.
 *   - NAPOT (RP2350's Hazard3): page-sized, naturally aligned blocks that
 *     never spill past a piece's top, so a page shared by code and data
 *     becomes RWX and `.rodata` protection needs the linker's help.
 *   - PMSAv8 (RP2350's M33): base + limit at 32 bytes, so the bounds are
 *     followed exactly and the guard hole is even split into a landing
 *     strip for exception entry.
 *   - ARMv7-M (M0+, M3): naturally aligned power-of-two regions; when the
 *     precise split does not fit the region budget the whole window becomes
 *     one RWX region, with a message saying why.
 */
void plat_task_protect(const plat_seg_t *segs, int n);

/* Bytes the kernel keeps unclaimed between the top of the heap and the
 * bottom of the stack: process_sbrk refuses to grow the break past
 * stack_low - PLAT_HEAP_GUARD, and ports with a real protection unit deny
 * user access to the whole hole.  Big enough to catch a runaway recursion
 * deeper than one page, small enough to leave most of the window to the
 * heap.
 *
 * Per board, like the stack size: a 112 MiB user area on the PC and a
 * 192 KiB one on the C3 cannot share one answer.  It is the floor of a
 * window's fixed cost, so on a small board it is what decides whether a
 * 32 KiB window is 12 KiB of image or 4. */
extern const uint32_t PLAT_HEAP_GUARD;

/* Bytes of stack at the top of a task window, for tasks that do not ask for
 * anything else.
 *
 * A program can ask for a different size with TOS_STACK() in its own source
 * (user/lib.h): the number travels in its image as an ELF note, exec() reads
 * it before sizing the window, and keeps it only if the port can describe it
 * - a power of two, at least one page, that divides PLAT_APP_GRAIN.  That is
 * the whole vocabulary the hardware has: rv32 encodes the stack as one NAPOT
 * PMP entry (power of two, aligned to its own size) and an ARMv7-M MPU region
 * has the same shape.  Because every window starts on a grain boundary and is
 * a whole number of grains long, a stack size that divides the grain is
 * always aligned to itself without the allocator doing anything.
 *
 * The DEFAULT has to satisfy all of these, or the layout check in
 * process_init() panics at boot:
 *
 *   - it is a power of two,
 *   - PLAT_APP_GRAIN is a multiple of it,
 *   - PLAT_APP_GRAIN - PLAT_STACK_BYTES leaves room for PLAT_HEAP_GUARD and
 *     at least one page of image,
 *   - PLAT_APP_BASE is aligned to it.
 *
 * The last two are what make a single NAPOT PMP entry describe the stack
 * exactly.  The C3's windows start on a 16 KiB boundary, because the linker
 * aligns __app_base to PLAT_APP_GRAIN - so a 32 KiB stack would encode as the
 * 32 KiB region *containing* the stack instead: the guard page below it
 * becomes writable and the upper half of the real stack does not (found on
 * the real chip by leaving one PMP rule out of a test program).  The same
 * test is why TOS_STACK() cannot ask for more than the grain.
 *
 * The default is the floor of every window, which is why it should be small:
 * measured high-water marks (ps's stk column) are under 1 KiB for every
 * program shipped, so 8 KiB on the small boards leaves a program that needs
 * more to say so, and a program that needs less to run in half the window. */
extern const uint32_t PLAT_STACK_BYTES;

/* Register frame captured when trapping from user mode.  The layout is
 * architecture specific, so each port supplies it (arch/<arch>/regs.h);
 * core code only ever touches it through the plat_frame_* helpers below. */
#include "regs.h"

/* Architecture-neutral view of a syscall trap.  Core code never reads
 * eax/ebx/ecx/edx itself: the arch maps its own convention onto these
 * fields (i386 ebx/ecx/edx, rv32 a0/a1/a2, arm r0/r1/r2). */
typedef struct {
    uint32_t nr;          /* syscall number */
    uint32_t a0, a1, a2;  /* arguments */
} syscall_t;

void     plat_frame_get_syscall(const regs_t *r, syscall_t *sc);
void     plat_frame_set_ret(regs_t *r, uint32_t val);
uint32_t plat_frame_get_ret(const regs_t *r);

/* Prepare a trap frame that starts a user program at `entry` with stack
 * `sp`.  Owns all architecture specific segment/CSR setup. */
/* First argument of a user entry point (the thread trampoline takes one).
 * Called right after plat_prepare_user_frame(). */
void     plat_frame_set_arg(regs_t *frame, uint32_t arg);

void     plat_prepare_user_frame(regs_t *frame, uint32_t entry, uint32_t sp);

/* The same for a kernel thread: a frame that starts `fn(arg)` on the given
 * pool stack, in Thread mode but with the kernel's privileges.  Implemented
 * by the ports that can run kernel threads (rv32 keeps its M-mode bootstrap
 * in process.c; arm uses privileged Thread mode on PSP). */
void     plat_prepare_kernel_frame(regs_t *frame, uint32_t entry,
                                   uint32_t a0, uint32_t a1,
                                   uint32_t stack_top);

/* 1 when the caller runs with the kernel's privileges (a kernel thread on
 * ports that have them).  Diagnostics only: the ktask selftest reports it so
 * "the thread really is privileged" is a measurement, not a reading. */
int      plat_thread_is_privileged(void);

/* Power the machine off (best effort). */
void     plat_shutdown(void) __attribute__((noreturn));

#define PLAT_PAGE_SIZE 4096u


/* Stack pointer for the kernel side of a trap taken from user mode. */
void plat_set_kernel_stack(uint32_t top);

/* Drop to user mode with the supplied register frame. */
void plat_enter_user(void *frame) __attribute__((noreturn));

/* Did this trap come from user mode?  The scheduler only switches on a
 * user frame: a trap that landed inside a kernel call has to let that call
 * finish, because the kernel stack below it is not resumable. */
int plat_fault_is_user(void *frame);

/* The user stack pointer this frame was taken with.  A port with no faulting
 * protection unit (RP2040, PC) uses it to notice a stack that ran off its
 * bottom, because nothing else will. */
uint32_t plat_frame_get_user_sp(const regs_t *r);

/* Stop the machine. */
void plat_panic_halt(void) __attribute__((noreturn));

/* Board hook polled inside plat_panic_halt().  Left NULL by boards that
 * have no way to be recovered remotely; the rp2350 port installs a Ctrl-D
 * poll so a crashed kernel still drops back into the ROM bootloader. */
extern void (*g_panic_poll)(void);

/* Board hook for the shared USBCTRL (IRQ 14) vector.  NULL on boards that
 * have no such controller; the rp2350 port installs its CDC driver's event
 * pump so a SET_ADDRESS status stage is answered within microseconds. */
extern void (*g_usb_irq_handler)(void);

/* Reset the machine and boot again (SYS_REBOOT, the shell's `reboot`).
 * Noreturn by contract: a port that cannot reset halts instead, so the
 * caller never resumes.  Implementations drain the console first. */
void plat_reboot(void) __attribute__((noreturn));

#endif /* TINYOS_PLAT_MEMPROT_H */

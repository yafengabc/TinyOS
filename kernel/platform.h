#ifndef TINYOS_PLATFORM_H
#define TINYOS_PLATFORM_H

#include "types.h"

/* Platform abstraction layer.
 *
 * kernel/core/ may only call these functions; it must never touch port I/O,
 * control registers, CSR registers or any other architecture specific state.
 * Each arch/ port implements this header, and each board/ supplies the
 * memory layout constants. */

/* ---- memory layout (provided by the board) --------------------------- */

/* Where the kernel image was linked and may occupy.  The kernel's .bss
 * (including the 32 KiB heap arena) must end below PLAT_KERNEL_MAX; there
 * is no frame allocator, so this is purely a sanity bound. */
extern const uint32_t PLAT_KERNEL_BASE;
extern const uint32_t PLAT_KERNEL_MAX;

/* Where user programs are loaded (position independent images are
 * relocated here).  Identical binaries work on every board. */
extern const uint32_t PLAT_APP_BASE;
extern const uint32_t PLAT_APP_MAX;

/* Bytes reserved per task inside [APP_BASE, APP_MAX).  Each task gets its
 * own window - image at the bottom, sbrk heap above it, stack at the top -
 * so a synchronous exec never overwrites its parent.  The flat model has no
 * page tables, so these windows are carved out by the kernel itself. */
extern const uint32_t PLAT_APP_WINDOW;

/* ---- boot ------------------------------------------------------------ */

/* Runs before anything else: sets up the stack and enough of the console
 * to be able to report a panic. */
void plat_early_init(void);

/* Full initialisation: interrupt controller, timer, console, block device. */
void plat_init(void);

/* ---- console --------------------------------------------------------- */

void plat_console_init(void);
void plat_console_putc(char c);
int  plat_console_getchar(void);      /* -1 when no input is pending */

/* ---- timer / interrupts ---------------------------------------------- */

/* Programme the periodic tick.  plat_ticks() must advance at `hz`. */
void     plat_timer_init(uint32_t hz);
uint64_t plat_ticks(void);
uint32_t plat_tick_hz(void);

void plat_irq_enable(void);
void plat_irq_disable(void);
void plat_cpu_idle(void);             /* hlt / wfi */

/* Used while waiting for console input: enable interrupts briefly and let
 * the CPU rest, but do not block for a whole tick. */
void plat_cpu_idle_poll(void);

/* ---- time ------------------------------------------------------------ */

uint32_t plat_uptime_ms(void);
void     plat_sleep_ms(uint32_t ms);

/* ---- block device (backing store for littlefs) ----------------------- */

int      plat_block_read(uint32_t lba, uint32_t count, void *buf);
int      plat_block_write(uint32_t lba, uint32_t count, const void *buf);
int      plat_block_erase(uint32_t lba);   /* writes the erased value */
uint32_t plat_block_size(void);            /* bytes per sector */
uint32_t plat_block_count(void);

/* ---- physical memory ------------------------------------------------- */

/* The flat model needs no frame allocator or page tables: every byte of RAM
 * is directly addressable. */

/* ---- user mode ------------------------------------------------------- */

/* There is no address-space abstraction: the unified model is a single flat
 * address space with no memory protection, matching the MCUs this OS targets.
 * Kernel code validates user pointers against the current task's window
 * (process_user_ok), and reads/writes them directly. */

/* Register frame captured when trapping from user mode.  The layout is
 * architecture specific; this matches the i386 port. */
typedef struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t vector, err;
    uint32_t eip, cs, eflags, useresp, ss;
} regs_t;

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
void     plat_prepare_user_frame(regs_t *frame, uint32_t entry, uint32_t sp);

/* Power the machine off (best effort). */
void     plat_shutdown(void) __attribute__((noreturn));

#define PLAT_PAGE_SIZE 4096u


/* Stack pointer for the kernel side of a trap taken from user mode. */
void plat_set_kernel_stack(uint32_t top);

/* Drop to user mode with the supplied register frame. */
void plat_enter_user(void *frame) __attribute__((noreturn));

/* Did this trap come from user mode? */
int plat_fault_is_user(void *frame);

/* Stop the machine. */
void plat_panic_halt(void) __attribute__((noreturn));

/* Reboot the machine (used after a fatal error, may be a no-op). */
void plat_reboot(void) __attribute__((noreturn));

#endif
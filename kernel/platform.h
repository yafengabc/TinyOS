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
/* How many characters plat_console_putc() will take right now without
 * waiting for the transmitter.  Keystroke echo uses this to stay out of the
 * way: echo is produced inside the tick, and a blocking write there delays
 * the next read of the input FIFO - a burst longer than one tick's worth of
 * line time (about 115 bytes at 115200) then overruns a 128-byte receive
 * FIFO and characters are lost.  Return 0 when nothing fits. */
int  plat_console_tx_room(void);
/* Wait until everything written has actually been transmitted.  Called once
 * on the way down: the device may buffer, and the CPU is about to stop. */
void plat_console_flush(void);
int  plat_console_getchar(void);      /* -1 when no input is pending */

/* ---- timer / interrupts ---------------------------------------------- */

/* Programme the periodic tick.  plat_ticks() must advance at `hz`. */
void     plat_timer_init(uint32_t hz);
uint64_t plat_ticks(void);
uint32_t plat_tick_hz(void);

void plat_irq_enable(void);
void plat_irq_disable(void);
void plat_cpu_idle(void);             /* hlt / wfi, interrupts enabled */

/* The arch dispatcher reports an external interrupt as a CPU line number.
 * Exactly one of them is the clock; the rest belong to the board (the C3
 * routes console input to its own line so a byte is drained the moment it
 * arrives instead of at the next tick - ten milliseconds is 115 bytes at
 * 115200, which leaves almost no room in a 128-byte receive FIFO). */
int  plat_irq_is_tick(uint32_t line);
/* Acknowledge a non-tick line after its work is done.  Some sources latch:
 * the UART's receive-timeout bit stays set until written back, and leaving
 * it set means the interrupt fires again the moment we return - the CPU
 * spends all its time in the handler and no task ever runs. */
void plat_irq_service(uint32_t line);

/* ---- time ------------------------------------------------------------ */

uint32_t plat_uptime_ms(void);

/* ---- block device (backing store for littlefs) ----------------------- */

/* First block of the littlefs partition.  The PC port writes the
 * filesystem right after the kernel image on the boot disk (181); boards
 * that embed the image in RAM start at 0. */
extern const uint32_t PLAT_LFS_LBA;

int      plat_block_read(uint32_t lba, uint32_t count, void *buf);
int      plat_block_write(uint32_t lba, uint32_t count, const void *buf);
int      plat_block_erase(uint32_t lba, uint32_t count);
                                     /* make count sectors read the erased
                                      * value: a real NOR erase where the
                                      * medium needs one, an 0xFF fill
                                      * where plain writes are safe */
uint32_t plat_block_size(void);            /* bytes per sector */
uint32_t plat_block_count(void);

/* ---- physical memory ------------------------------------------------- */

/* The flat model needs no frame allocator or page tables: every byte of RAM
 * is directly addressable. */

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

/* Program the protection for the task that is about to run, then forget
 * about it: memory protection is a property of the running task, not of
 * the kernel.
 *
 *   [win_base, wx)          code            execute + read
 *   [wx, brk)               data + heap     read + write, brk grows via sbrk
 *   [stack_low, stack_top)  stack           read + write
 *
 * Everything else - the kernel's own memory, MMIO, the rest of the app
 * window - is out of reach for user mode, including the hole between brk
 * and stack_low that neither region claims.  `stack_top - stack_low` must
 * be a power of two and stack_low aligned to it.
 *
 * How closely a port can follow those bounds is its own business: an
 * ARMv7-M MPU only has naturally aligned power-of-two regions, so there the
 * heap is not cut at `brk` (it ends at the last block the window region
 * covers) and only PLAT_HEAP_GUARD bytes under the stack are a hole (see
 * docs/PLATFORM-DESIGN.md, section 8). */
void plat_task_protect(uint32_t win_base, uint32_t wx, uint32_t brk,
                       uint32_t stack_low, uint32_t stack_top);

/* Bytes the kernel keeps unclaimed between the top of the heap and the
 * bottom of the stack: process_sbrk refuses to grow the break past
 * stack_low - PLAT_HEAP_GUARD, and ports with a real protection unit deny
 * user access to the whole hole.  Big enough to catch a runaway recursion
 * deeper than one page, small enough to leave most of the window to the
 * heap.
 *
 * Per board, like the stack size: a 4 MiB window on the PC and a 32 KiB one
 * on the RP2040 cannot share one answer. */
extern const uint32_t PLAT_HEAP_GUARD;

/* Bytes of stack at the top of each task window.
 *
 * A board must pick a value for which ALL of these hold, or the task layout
 * check in process_init() panics at boot:
 *
 *   - it is a power of two,
 *   - PLAT_APP_WINDOW is a multiple of it,
 *   - PLAT_APP_WINDOW - PLAT_STACK_BYTES leaves room for the image, the heap
 *     and PLAT_HEAP_GUARD,
 *   - PLAT_APP_BASE + PLAT_APP_WINDOW - PLAT_STACK_BYTES is aligned to it.
 *
 * The last one is what makes a single NAPOT PMP entry describe the stack
 * exactly.  The C3's windows start at 0x403AC000 - 16 KiB aligned, because
 * the kernel ends there - so a 32 KiB stack would encode as the 32 KiB
 * region *containing* the stack instead: the guard page below it becomes
 * writable and the upper half of the real stack does not (found on the real
 * chip by leaving one PMP rule out of a test program). */
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
void     plat_prepare_user_frame(regs_t *frame, uint32_t entry, uint32_t sp);

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

/* Stop the machine. */
void plat_panic_halt(void) __attribute__((noreturn));

/* Reset the machine and boot again (SYS_REBOOT, the shell's `reboot`).
 * Noreturn by contract: a port that cannot reset halts instead, so the
 * caller never resumes.  Implementations drain the console first. */
void plat_reboot(void) __attribute__((noreturn));

#endif
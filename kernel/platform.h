#ifndef TINYOS_PLATFORM_H
#define TINYOS_PLATFORM_H

#include "types.h"
#include "tos.h"      /* TOS_PIN_*, so the GPIO modes have one definition */

/* Platform abstraction layer.
 *
 * kernel/core/ may only call these functions; it must never touch port I/O,
 * control registers, CSR registers or any other architecture specific state.
 * Each arch/ port implements this header, and each board/ supplies the
 * memory layout constants. */

/* ---- memory layout (provided by the board) --------------------------- */

/* Where the kernel image was linked.  There is no frame allocator: the
 * kernel owns [PLAT_KERNEL_BASE, __kernel_end) and the task area starts right
 * after it. */
extern const uint32_t PLAT_KERNEL_BASE;

/* First address a task window may use.  The linker rounds the kernel's end up
 * to PLAT_APP_GRAIN and exports it as __app_base, so shrinking the heap, the
 * kernel stack or the process table automatically enlarges the task area
 * instead of leaving a fixed gap.  Identical binaries still run on every
 * board: only the address they are relocated to changes. */
extern uint8_t __app_base[];
#define PLAT_APP_BASE ((uint32_t)__app_base)
extern const uint32_t PLAT_APP_MAX;

/* Allocation granularity of task windows inside [APP_BASE, APP_MAX).
 *
 * A window is one task's slice of the flat address space: image at the
 * bottom, sbrk heap above it, stack at the top, so a synchronous exec never
 * overwrites its parent.  Its size is not fixed: exec() measures the image
 * (span), adds the stack and the guard hole, rounds up to this grain, and
 * asks for that much.  A shell takes one or two grains, an editor three or
 * four, and what is left over goes to whoever comes next - the flat model
 * has no page tables, so the kernel carves these out itself.
 *
 * The grain has to be a multiple of PLAT_STACK_BYTES: that is what keeps
 * every window's stack_low aligned to the stack size, which is what lets a
 * single NAPOT PMP entry describe the stack.  It also has to be big enough
 * to hold a stack, a guard hole and at least one page of image. */
extern const uint32_t PLAT_APP_GRAIN;

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

/* Arm this CPU's own periodic interrupt.  On a uniprocessor this is a no-op
 * (plat_timer_init already did it); on SMP it is what a secondary hart calls
 * before it enters the scheduler, because the timer compare register and the
 * interrupt-enable CSR are per hart - hart 1 inherits nothing from hart 0. */
void plat_timer_start_cpu(void);

/* Bring CPU `id` out of reset.  Boards whose secondaries are already
 * spinning on cpu_release (qemu-virt) leave the weak default in place and
 * return 0; boards where the second core sits in a boot-ROM wait loop
 * (RP2350's SIO FIFO) implement it.  Called once per CPU from cpu_init(),
 * after the process table, the timer and the console exist. */
int  plat_start_cpu(int id);

void plat_irq_enable(void);
void plat_irq_disable(void);
void plat_cpu_idle(void);             /* hlt / wfi, interrupts enabled */

/* Save the interrupt-enable state and disable interrupts.  plat_irq_restore()
 * puts back exactly what was there.
 *
 * A critical section that can be entered both from a system call (interrupts
 * on) and from the tick handler (interrupts already off) must not re-enable
 * on the way out, so disable()/enable() are the wrong pair; and on ARM
 * plat_irq_enable() has a side effect - it unmasks the board's tick source
 * the first time it runs on a core.  See docs/OPEN-ITEMS.md §23.3. */
uint32_t plat_irq_save(void);
void     plat_irq_restore(uint32_t state);

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

/* ---- cpu topology ----------------------------------------------------- */

/* How many CPUs this build supports.  The build passes -DTOS_MAX_CPUS=2 for
 * the SMP target (qemu-virt with -smp 2) and nothing for the others; the
 * fallback keeps every `#if TOS_MAX_CPUS > 1` in the tree honest without a
 * second macro.  start.S reads the same define, which is how the trap
 * vectors and the kernel-stack blocks stay in step with it. */
#ifndef TOS_MAX_CPUS
#define TOS_MAX_CPUS 1
#endif

/* How many CPUs the kernel was built for.  Every board but one is a
 * uniprocessor and sets 1; the SMP build (rv32smp) sets 2 and boots the
 * secondaries from start.S.  This is a compile-time constant, not a runtime
 * probe: with no MMU there is nothing to allocate per CPU at runtime except
 * a kernel stack, and the linker reserves exactly that many.
 *
 * It is a separate symbol from TOS_MAX_CPUS on purpose: the board owns the
 * answer (layout.c), the build only says what it is, and a board that
 * cannot do SMP simply keeps saying 1 whatever the build asked for. */
extern const uint32_t PLAT_MAX_CPUS;

/* Which CPU is asking.  Valid everywhere, including before the scheduler
 * exists (a secondary hart calls kmain-adjacent code with its own id). */
int plat_cpu_id(void);

/* ---- CPU description (for /proc/cpuinfo) ----------------------------- */

/* Core clock in Hz, or 0 when the port does not know it.  This is the CPU's
 * own clock, not the scheduler tick (plat_tick_hz) and not the bus clock. */
uint32_t plat_cpu_hz(void);

/* Human-readable core and ISA names.  The board owns model (the chip/core
 * combination); the architecture port owns isa because it is a property of
 * how this image was compiled, not of the board.  Both have weak defaults
 * that say "unknown" so a new board only has to fill in what it knows. */
const char *plat_cpu_model(void);
const char *plat_cpu_isa(void);

/* ---- time ------------------------------------------------------------ */

uint32_t plat_uptime_ms(void);

/* Monotonic nanoseconds since boot.  The tick above is what the scheduler
 * counts; this is the clock: it comes from a free-running hardware counter
 * (CLINT mtime on qemu-virt, the TG0 time base on the ESP32-C3, SysTick plus
 * its live down-counter on Cortex-M) and has sub-tick resolution, so a
 * program can time something shorter than the 10 ms quantum.
 *
 * Boards that only have the tick fall back to `plat_ticks() * period`, which
 * is monotonic too - just coarser.  The value never goes backwards, even if
 * the underlying counter is reloaded while the tick is being serviced. */
uint64_t plat_now_ns(void);

/* ---- block device (backing store for littlefs) ----------------------- */

/* First block of the littlefs partition.  The PC port writes the
 * filesystem right after the kernel image on the boot disk (181); boards
 * that embed the image in RAM start at 0. */
extern const uint32_t PLAT_LFS_LBA;

/* Number of littlefs blocks in the partition.  Board-specific (each
 * board's layout.c sets it) so the C3 can grow its 4 MiB flash partition
 * without dragging the other boards up with it.  The kernel mounts this
 * many blocks of FS_BLOCK_SIZE starting at PLAT_LFS_LBA. */
extern const uint32_t PLAT_LFS_BLOCKS;

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

/* Start of usable RAM.  On most boards this is PLAT_KERNEL_BASE, but not on
 * all: the mps2 links its code at 0 while .data/.bss live in SRAM at
 * 0x20000000, so the two are different numbers there. */
extern const uint32_t PLAT_RAM_BASE;

/* One past the last byte of RAM the kernel may touch.  Boards differ by
 * more than the size of the SRAM: on the C3 the top few KiB are the ROM's
 * own download buffers and stack, and are not ours to use.  `free` reports
 * the map from PLAT_RAM_BASE to here, so the number a user sees is the
 * memory that actually exists, not the size of the kernel's heap arena. */
extern const uint32_t PLAT_RAM_TOP;

/* End of the kernel's .bss, from the linker script.  `free` uses it to say
 * how much RAM the kernel itself occupies. */
extern char __bss_end[];

/* ---- gpio / adc ------------------------------------------------------ */

/* Digital I/O.  Pins are numbered the way the board numbers them (the C3's
 * GPIO0..21, the RP2040's GPIO0..29); there is no port prefix because no
 * board here has more than one bank.
 *
 * A board that has no GPIO at all just does not define these: kernel/periph.c
 * carries weak defaults that report "unsupported", so a program asks
 * plat_gpio_count() first and prints a sentence instead of failing. */
#define PLAT_GPIO_IN          TOS_PIN_IN
#define PLAT_GPIO_OUT         TOS_PIN_OUT
#define PLAT_GPIO_IN_PULLUP   TOS_PIN_IN_PULLUP
#define PLAT_GPIO_IN_PULLDOWN TOS_PIN_IN_PULLDOWN

int plat_gpio_count(void);                       /* 0 = no GPIO on this board */
int plat_gpio_configure(int pin, int mode);      /* 0 or -1                  */
int plat_gpio_write(int pin, int value);         /* 0 or -1                  */
int plat_gpio_read(int pin);                     /* 0/1, or -1               */

/* Analog input.  plat_adc_read() returns raw counts in [0, 2^bits) and -1
 * when the channel does not exist or the conversion failed; the two
 * constants describe the scale so user space can turn counts into
 * millivolts without knowing the part. */
int plat_adc_count(void);
int plat_adc_read(int chan);
int plat_adc_vref_mv(void);
int plat_adc_bits(void);

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
 * Everything else - the kernel's own memory, MMIO, other tasks' windows -
 * is out of reach for user mode, including the hole between brk and
 * stack_low that neither region claims.  `stack_top - stack_low` must be a
 * power of two and stack_low aligned to it.
 *
 * `win_end` is the end of this task's window.  It varies per task now that
 * windows are sized to their image, and only matters to a port that
 * describes the whole window rather than the three regions inside it (the
 * ARMv7-M MPU, one region for "everything this task may touch"); rv32's
 * TOR entries stop at wx and brk and ignore it.
 *
 * How closely a port can follow those bounds is its own business: an
 * ARMv7-M MPU only has naturally aligned power-of-two regions, so there the
 * heap is not cut at `brk` (it ends at the last block the window region
 * covers) and only PLAT_HEAP_GUARD bytes under the stack are a hole (see
 * docs/PLATFORM-DESIGN.md, section 8). */
void plat_task_protect(uint32_t win_base, uint32_t win_end, uint32_t wx,
                       uint32_t brk, uint32_t stack_low, uint32_t stack_top);

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

#endif
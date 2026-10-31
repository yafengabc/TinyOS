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

/* Where the kernel image was linked and may occupy. */
extern const uint32_t PLAT_KERNEL_BASE;
extern const uint32_t PLAT_KERNEL_MAX;

/* First and last address managed by the page/frame allocator. */
extern const uint32_t PLAT_HEAP_BASE;
extern const uint32_t PLAT_HEAP_END;

/* Where user programs are loaded (position independent images are
 * relocated here).  Identical binaries work on every board. */
extern const uint32_t PLAT_APP_BASE;
extern const uint32_t PLAT_APP_MAX;

/* Top of the user stack; grows downward from here. */
extern const uint32_t PLAT_APP_STACK_TOP;

/* Base of the sbrk heap; grows upward from here. */
extern const uint32_t PLAT_APP_HEAP_BASE;

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

/* ---- user memory validation ------------------------------------------ */

int      plat_user_ok(uint32_t as, uint32_t addr, uint32_t len);

/* ---- block device (backing store for littlefs) ----------------------- */

int      plat_block_read(uint32_t lba, uint32_t count, void *buf);
int      plat_block_write(uint32_t lba, uint32_t count, const void *buf);
int      plat_block_erase(uint32_t lba);   /* writes the erased value */
uint32_t plat_block_size(void);            /* bytes per sector */
uint32_t plat_block_count(void);

/* ---- physical memory ------------------------------------------------- */

void     plat_mem_init(void);
uint32_t plat_alloc_page(void);       /* 4 KiB aligned, 0 on failure */
void     plat_free_page(uint32_t p);

/* ---- address translation (for the ELF loader) ------------------------ */

/* Translate a virtual address to a pointer the kernel can write through.
 * `as` is the address space handle (0 = the active one).  On a flat
 * platform (no MMU) this is the identity. */
uint32_t plat_translate(uint32_t as, uint32_t va);

/* Make [va, va+len) writable in `as`, allocating backing memory if needed.
 * Returns 0 on success. */
int      plat_map_range(uint32_t as, uint32_t va, uint32_t len);

/* ---- user mode ------------------------------------------------------- */

/* Register frame captured when trapping from user mode.  The layout is
 * architecture specific; this matches the i386 port. */
typedef struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t vector, err;
    uint32_t eip, cs, eflags, useresp, ss;
} regs_t;

/* Address space handle.  On x86 this is a page directory, on a flat
 * platform it is unused (0). */
uint32_t plat_as_create(void);
void     plat_as_destroy(uint32_t as);
uint32_t plat_current_as(void);

/* Back a user page at `va` with fresh writable memory. */
int      plat_user_page(uint32_t as, uint32_t va);

/* Switch to the given address space. */
void     plat_activate_as(uint32_t as);

/* Flush a TLB entry after changing a mapping. */
void     plat_flush_tlb(uint32_t va);

/* Prepare a trap frame that starts a user program at `entry` with stack
 * `sp`.  Owns all architecture specific segment/CSR setup. */
void     plat_prepare_user_frame(regs_t *frame, uint32_t entry, uint32_t sp);

/* Write to user memory through the address translation. */
void     plat_write_user(uint32_t as, uint32_t va, const void *src, uint32_t len);

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
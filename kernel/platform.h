#ifndef TINYOS_PLATFORM_H
#define TINYOS_PLATFORM_H

#include "types.h"
#include "tos.h"      /* TOS_PIN_*, so the GPIO modes have one definition */

/* Platform abstraction layer.
 *
 * kernel/core/ may only call these functions; it must never touch port I/O,
 * control registers, CSR registers or any other architecture specific state.
 * Each arch/ port implements this header, and each board/ supplies the
 * memory layout constants.
 *
 * Call-context rules (what may run in an ISR, under a lock, or with
 * interrupts off) are in docs/PLATFORM-CONTRACT.md; the header comments here
 * carry the per-interface detail. */

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


/* Narrow platform interfaces.  Each header keeps the declarations in
 * their original order; including platform.h still exposes every symbol. */
#include "plat/console.h"
#include "plat/irq.h"
#include "plat/block.h"
#include "plat/memprot.h"

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

/* PWM output.  The API is pin-based, not channel-based: a board says which
 * GPIO can produce PWM and the driver picks the slice.  Frequency is shared
 * by both channels of one slice on the RP2040; duty is per channel.  Boards
 * without PWM leave the weak defaults in kernel/periph.c, which report -1. */
int plat_pwm_count(void);                        /* 0 = no PWM on this board */
int plat_pwm_set(int gpio, int freq_hz, int duty_permille);
int plat_pwm_off(int gpio);

/* Extra serial ports, separate from the console (port 0 is UART1 on the
 * Pico, not UART0: UART0 already belongs to the console).  Reads are
 * non-blocking and return the bytes available, 0 when the FIFO is empty. */
int plat_uart_count(void);
int plat_uart_write(int port, const void *buf, int len);
int plat_uart_read(int port, void *buf, int len);

/* PIO (RP2040).  `pid` is the calling task, used for ownership: a state
 * machine loaded by one task is stopped and its pins released when that task
 * exits.  Boards without PIO leave the weak default in kernel/periph.c. */
int  plat_pio(int op, struct pio_req *req, int pid);
void plat_pio_release(int pid);

/* SPI master.  The driver owns SCK/MOSI/MISO for the selected bus; CS is a
 * plain GPIO because the PL022's hardware chip select is not wired on the
 * Pico and sharing one bus between devices needs software control anyway. */
int plat_spi_count(void);
int plat_spi_xfer(int bus, int cs_gpio, int freq_hz, int mode,
                  const void *tx, void *rx, int len);

/* I2C master, 7-bit addresses.  tx is written first, then rx is read with
 * a repeated START; a zero-length probe only checks the address. */
int plat_i2c_count(void);
int plat_i2c_xfer(int bus, int addr, int freq_hz,
                  const void *tx, int txlen, void *rx, int rxlen);

/* DMA memory-to-memory copy.  A board without DMA leaves the weak default
 * in kernel/periph.c, which returns -1. */
int plat_dma_memcpy(void *dst, const void *src, int len);

/* Flash/cache seams (C3/A-4).  begin/end are the only way kernel flash code
 * asks the port to park other cores; a board without parking leaves the weak
 * default (begin=0, end=no-op).  Cache hooks are no-ops on boards where the
 * CPU does not need explicit maintenance. */
int  plat_flash_exclusive_begin(void);
void plat_flash_exclusive_end(void);
void plat_icache_sync(void);
void plat_dcache_clean(const void *addr, unsigned len);
void plat_dcache_invalidate(const void *addr, unsigned len);
#define PLAT_CACHE_LINE 32


#endif

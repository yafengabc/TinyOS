#ifndef TINYOS_TOSAPI_H
#define TINYOS_TOSAPI_H

/* System call numbers, shared between the kernel and user programs. */

#define SYS_EXIT     0   /* exit(code)                          */
#define SYS_WRITE    1   /* write(fd, buf, len)                 */
#define SYS_READ     2   /* read(fd, buf, len)                  */
#define SYS_EXEC     3   /* exec(path, argv, argc)              */
#define SYS_OPEN     4   /* open(path)                          */
#define SYS_CLOSE    5   /* close(fd)                           */
#define SYS_READDIR  6   /* readdir(index, namebuf, len)        */
#define SYS_GETPID   7   /* getpid()                            */
#define SYS_SLEEP    8   /* sleep(ms)                           */
#define SYS_UPTIME   9   /* uptime() -> ms                      */
#define SYS_SBRK     10  /* sbrk(bytes) -> old break or -1      */
#define SYS_CREATE   11  /* create(path) -> fd (truncates)      */
#define SYS_WAIT     12  /* wait(pid) -> exit code, blocks      */
#define SYS_YIELD    13  /* yield() -> 0, give up the CPU       */
#define SYS_PS       14  /* ps() -> task count, prints a table  */
#define SYS_SIGNAL   15  /* signal(pid, sig) -> 0 or -1         */
#define SYS_REBOOT   16  /* reboot() -> does not return         */
#define SYS_SETPRIO  17  /* setprio(pid, prio) -> 0 or -1       */
#define SYS_STAT     18  /* stat(path, &st) -> 0 or -1          */
#define SYS_MEMINFO  19  /* meminfo(&m) -> 0, fills struct       */
#define SYS_DF       20  /* dfinfo(&d) -> 0, fills struct        */
#define SYS_CHDIR    21  /* chdir(path) -> 0 or -1               */
#define SYS_GETCWD   22  /* getcwd(buf, len) -> bytes or -1      */
#define SYS_TTYRAW   23  /* ttyraw(on) -> previous mode, 0/1     */
#define SYS_CLOCK    24  /* clockinfo(&c) -> 0, fills struct     */
#define SYS_GETCPU   25  /* getcpu() -> id of the CPU running us */
#define SYS_GPIO     26  /* gpio(op, pin, arg) -> depends on op  */
#define SYS_ADC      27  /* adc(op, chan) -> depends on op       */
#define SYS_PIPE     28  /* pipe(int fds[2]) -> 0 or -1          */
#define SYS_DUP2     29  /* dup2(old, new) -> new or -1          */
#define SYS_IOCTL    30  /* ioctl(fd, cmd, arg) -> depends on cmd */
#define SYS_UNLINK   31  /* unlink(path) -> 0 or -1             */
#define SYS_HIST     32  /* hist(op, idx, buf, cap) -> depends   */
#define SYS_LSEEK    33  /* lseek(fd, off, whence) -> new pos    */
#define SYS_FSTAT    34  /* fstat(fd, &st) -> 0 or -1            */
#define SYS_DUP      35  /* dup(fd) -> lowest free fd or -1      */
#define SYS_READDIR_AT 36 /* readdir_at(&req) -> name length or 0 */

/* SYS_POLL: readiness without blocking.  The timeout lives in user land
 * (sleep between calls), so this syscall never parks a task and the kernel
 * needs no wait queue for it. */
#define SYS_POLL     37

/* Mailbox: a small global pool of copied messages (TOS_MAILBOX builds).
 * Send is try-only; recv returns MSG_EAGAIN when the queue is empty, so the
 * blocking loop lives in user land (sleep 1 ms), exactly like poll(). */
#define SYS_MSG_SEND 38  /* msg_send(pid, buf, len) -> 0 or -1        */
#define SYS_MSG_RECV 39  /* msg_tryrecv(buf, len) -> bytes, -1, -2    */
#define SYS_PWM      40  /* pwm(op, arg) -> depends on op              */
#define SYS_UART_WRITE 41 /* uart_write(port, buf, len) -> bytes/-1    */
#define SYS_UART_READ  42 /* uart_read(port, buf, len) -> bytes/-1     */
#define SYS_UART_COUNT 43 /* uart_count() -> ports                     */
#define SYS_SPI      44  /* spi(op, &req) -> depends on op             */
#define SYS_I2C      45  /* i2c(op, &req) -> depends on op             */
#define SYS_DMA_MEMCPY 47 /* dma_memcpy(dst, src, len) -> len/-1       */
/* Kernel threads (TOS_KTHREADS): only built when the kthreads feature is on.
 * A thread shares the caller's window; `sp` is the top of a stack the caller
 * allocated inside it.  tid is a pid; join stores the exit code through the
 * user pointer. */
#define SYS_THREAD_CREATE 48 /* kthread_create(entry, arg, sp) -> tid  */
#define SYS_THREAD_EXIT   49 /* kthread_exit(code) -> no return        */
#define SYS_THREAD_JOIN   50 /* kthread_join(tid, &code) -> 0 or -1    */
/* Scheduler counters, for benchmarks: schedstat(&st) fills three u32
 * (switches to another task, scheduler runs that kept the same task, ticks). */
#define SYS_SCHEDSTAT     51
/* PIO (RP2040): pio(op, &req).  One request struct carries both the assembled
 * program and the SM configuration; see struct pio_req below. */
#define SYS_PIO           52

#define PIO_LOAD     0   /* req.buf = u16 words, req.len = count          */
#define PIO_START    1
#define PIO_STOP     2
#define PIO_TX       3   /* req.buf = u32 words, req.len = count          */
#define PIO_RX       4
#define PIO_RELEASE  5

#define PIO_F_SIDE_OPT     0x01   /* .side_set N opt                    */
#define PIO_F_SIDE_PINDIR  0x02   /* .side_set N pindirs                */

struct pio_req {
    int op;
    int block;          /* 0 or 1                                       */
    int sm;             /* 0..3                                         */
    const void *buf;    /* u16 instruction words (LOAD) or u32 data     */
    int len;            /* words: count for LOAD, TX and RX alike       */
    int origin;         /* instruction-memory slot for word 0           */
    int wrap_top;
    int wrap_bottom;
    int side_set;       /* side-set bit count, 0..5                     */
    int flags;          /* PIO_F_*                                      */
    int clkdiv_int;
    int clkdiv_frac;
    int pin_base;       /* first GPIO used, -1 = do not touch pins      */
    int pin_count;
    int shiftctrl;      /* extra SHIFTCTRL bits (autopull/autopush...)  */
};

#define MSG_EAGAIN   (-2)
#define TOS_MSG_MAX  256    /* bytes per pool slot (mailbox.h) */

/* One entry in a poll() set.  Same layout as user/lib.h's struct pollfd. */
struct tos_pollfd {
    int   fd;
    short events;       /* TOS_POLL* the caller cares about */
    short revents;      /* TOS_POLL* that are true right now */
};

#define TOS_POLLIN   0x001
#define TOS_POLLOUT  0x004
#define TOS_POLLERR  0x008
#define TOS_POLLNVAL 0x020

/* SYS_READDIR_AT: one name from `path`, without changing the cwd.  The cwd
 * version (SYS_READDIR) is what `ls` used to chdir for; userland's
 * opendir()/readdir() keeps one of these per open stream instead. */
struct tos_dirent_req {
    const char *path;   /* directory, resolved against the cwd    */
    int         index;  /* 0-based entry number                   */
    char       *name;   /* receives the name, NUL terminated      */
    int         cap;    /* bytes available at name                */
};

/* Signals.  There are no handlers: a signal either kills the task or parks
 * it, both decided by the kernel. */
#define SIGKILL 9       /* terminate now                        */
#define SIGSTOP 19      /* stop until SIGCONT                   */
#define SIGCONT 18      /* resume a stopped task                */

/* File types returned by stat() (struct stat lives in user/lib.h).  These are
 * ABI twice over: `ls` picks its letter from them, and SYS_CHDIR insists on
 * TOS_FTYPE_DIR.  A character device is its own type rather than a regular
 * file of size 0, because "open it and read it" and "read this file" are
 * different promises. */
#define TOS_FTYPE_REG 0   /* regular file                        */
#define TOS_FTYPE_DIR 1   /* directory                           */
#define TOS_FTYPE_CHR 2   /* character device (/dev, /proc)      */
#define TOS_FTYPE_UNKNOWN 255 /* readdir() could not stat the entry */

/* Pin modes for gpio_mode(), and the op codes the two multiplexed
 * peripheral syscalls take.  These are ABI: the kernel's periph.h maps them
 * onto the board's plat_gpio_* / plat_adc_* hooks. */
#define TOS_PIN_IN           0
#define TOS_PIN_OUT          1
#define TOS_PIN_IN_PULLUP    2
#define TOS_PIN_IN_PULLDOWN  3

#define GPIO_OP_COUNT  0   /* ()          -> number of pins              */
#define GPIO_OP_MODE   1   /* (pin, mode) -> 0 or -1                     */
#define GPIO_OP_WRITE  2   /* (pin, 0|1)  -> 0 or -1                     */
#define GPIO_OP_READ   3   /* (pin)       -> 0/1, -1 if not readable     */

#define ADC_OP_COUNT   0   /* ()     -> number of channels               */
#define ADC_OP_READ    1   /* (chan) -> raw counts, -1 on failure        */
#define ADC_OP_VREF    2   /* ()     -> reference in millivolts          */
#define ADC_OP_BITS    3   /* ()     -> resolution in bits               */

/* PWM is addressed by GPIO, not by an abstract channel: on the RP2040 every
 * pin 0..29 can be routed to one of the 16 slice outputs.  The frequency is
 * a property of the slice, so two pins in the same slice share it. */
#define PWM_OP_COUNT   0   /* ()     -> number of PWM channels           */
#define PWM_OP_SET     1   /* (&req) -> 0, configure gpio/freq/duty      */
#define PWM_OP_OFF     2   /* (gpio) -> 0, stop and release the pin      */

struct tos_pwm_req {
    int gpio;
    int freq_hz;
    int duty_permille;      /* 0..1000 */
};

/* SPI is a full-duplex byte transfer.  `tx` and `rx` may each be NULL; a
 * missing tx sends 0xFF, a missing rx discards the received bytes.  `cs_gpio`
 * is driven by software, so the caller can use any free pin; -1 means the
 * caller owns the chip select. */
#define SPI_OP_COUNT   0   /* ()     -> number of SPI buses             */
#define SPI_OP_XFER    1   /* (&req) -> bytes transferred, -1            */
struct tos_spi_req {
    int bus;                    /* 0 = SPI0, 1 = SPI1                */
    int cs_gpio;                /* -1 = no software chip select      */
    int freq_hz;
    int mode;                   /* 0..3: CPOL/CPHA                   */
    const unsigned char *tx;
    unsigned char *rx;
    int len;
};

/* I2C master.  A zero-length transfer with no tx/rx is an address probe
 * (used by `i2c scan`).  tx is written first, then rx is read, with a
 * repeated START between the two when both are present. */
#define I2C_OP_COUNT   0   /* ()     -> number of I2C buses             */
#define I2C_OP_XFER    1   /* (&req) -> 0/-1 for probe, bytes otherwise */
struct tos_i2c_req {
    int bus;                    /* 0 = I2C0, 1 = I2C1                */
    int addr;                   /* 7-bit address                     */
    int freq_hz;
    const unsigned char *tx;
    int txlen;
    unsigned char *rx;
    int rxlen;
};

/* ---- on-board devices ------------------------------------------------
 *
 * A board is not just a CPU: it has a LED, a button, a panel, a card slot.
 * Each of those is described once, in the board's device table, with a
 * class and its pins; /proc/dev lists the table and the generic drivers
 * (kernel/hwdev.c) turn the classes into /dev nodes.  A class number is
 * ABI: user space prints a name from it, so the numbers are append-only.
 *
 * The point of naming the class rather than the part is that `led0` means
 * the same thing on every board even though one board lights its LED high
 * and the next lights it low - polarity is a board fact, hidden here. */
#define TOS_DEV_LED       1   /* a user-visible LED, on/off             */
#define TOS_DEV_KEY       2   /* a button, readable while pressed       */
#define TOS_DEV_DISPLAY   3   /* a text-capable panel (/dev/lcd0)       */
#define TOS_DEV_STORAGE   4   /* removable card slot                    */
#define TOS_DEV_UART      5   /* a wired serial port                    */
#define TOS_DEV_USB       6   /* the native USB port                    */
#define TOS_DEV_ADC       7   /* an analog input pin                    */
#define TOS_DEV_FLASH     8   /* the boot flash part                    */
#define TOS_DEV_GPIO      9   /* a pin broken out on a header           */
#define TOS_DEV_SENSOR   10   /* anything else worth naming             */
#define TOS_DEV_SPI      11   /* SPI controller / header bus            */
#define TOS_DEV_I2C      12   /* I2C controller / header bus            */
#define TOS_DEV_PWM      13   /* PWM slice bank                         */
#define TOS_DEV_PIO      14   /* programmable I/O block                 */
#define TOS_DEV_DMA      15   /* DMA controller                         */
#define TOS_DEV_PLL      16   /* clock generator                        */
#define TOS_DEV_DIV      17   /* integer divider                        */
#define TOS_DEV_INTERP   18   /* interpolator                           */
#define TOS_DEV_LDO      19   /* on-chip regulator                      */
#define TOS_DEV_SWD      20   /* debug access port                      */

/* Flags in a device-table entry. */
#define TOS_DEV_ACTIVE_LOW (1u << 0)  /* asserting the pin means 0         */
#define TOS_DEV_BOUND      (1u << 1)  /* a driver claimed it this boot     */
#define TOS_DEV_SHARED     (1u << 2)  /* shares pins with another device   */

/* The screen, as a character device.  write() takes UTF-8-ish ASCII text
 * and renders it at the cursor, handling '\n', '\r' and '\b'; ioctl()
 * covers everything that is not text.  Colors are RGB565, the format the
 * panel takes, so a caller never converts. */
#define LCD_OP_INFO        0  /* ()            -> (cols << 16) | rows     */
#define LCD_OP_CLEAR       1  /* (rgb565)      -> 0, clears to that color */
#define LCD_OP_CURSOR      2  /* ((row<<16)|col) -> 0                     */
#define LCD_OP_BACKLIGHT   3  /* (0|1)         -> 0, previous state       */
#define LCD_OP_MIRROR      4  /* (0|1)         -> 0, previous state       */
#define LCD_OP_INVERT      5  /* (0|1)         -> 0, swaps fg/bg          */
#define LCD_OP_FG          6  /* (rgb565)      -> previous foreground     */
#define LCD_OP_BG          7  /* (rgb565)      -> previous background     */
#define LCD_OP_PIXELS      8  /* ()            -> (width << 16) | height  */
#define LCD_OP_DEVS        9  /* ()            -> 0, draws the device map */
#define LCD_OP_MIRROR_GET 10  /* ()            -> current mirror state    */

/* The same panel as a pixel framebuffer, /dev/fb0.  ioctl(FB_OP_RECT)
 * selects where the next write() lands; write() takes a stream of RGB565
 * pixels, two bytes each, high byte first (the order the ST7789 takes).
 * A short write just fills the first rows of the rectangle. */
#define FB_OP_INFO         0  /* ()            -> (width << 16) | height  */
#define FB_OP_RECT         1  /* packed x/y/w/h below -> 0               */
#define FB_OP_SYNC         2  /* ()            -> 0, wait for the DMA tail */
#define FB_OP_CLEAR        3  /* (rgb565)      -> 0, fill whole panel      */

/* FB_OP_RECT argument: four byte-sized fields, which is plenty for a
 * 240x135 panel and keeps the generic ioctl signature (one int) intact. */
#define FB_RECT(x, y, w, h) (((x) & 0xffu) | (((y) & 0xffu) << 8) |  \
                             (((w) & 0xffu) << 16) | (((h) & 0xffu) << 24))
#define FB_RECT_X(a) ((a) & 0xff)
#define FB_RECT_Y(a) (((a) >> 8) & 0xff)
#define FB_RECT_W(a) (((a) >> 16) & 0xff)
#define FB_RECT_H(a) (((a) >> 24) & 0xff)

#endif

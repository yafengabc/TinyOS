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

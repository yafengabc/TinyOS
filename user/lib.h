#ifndef TINYOS_USER_LIB_H
#define TINYOS_USER_LIB_H

#include "types.h"
#include "tos.h"      /* TOS_PIN_*, the pin modes gpio_mode() takes */

/* How much stack this program needs, when the board's default is not it.
 *
 *     TOS_STACK(16 * 1024);      -- at file scope, once per program
 *
 * The number travels inside the image as an ELF note (user.ld gives .note.tos
 * a PT_NOTE segment of its own) and the kernel reads it *before* it sizes the
 * task window, so this is not a reservation made at run time - it decides how
 * big a window the program gets at all.  A program that recurses (a parser
 * walking a tree, an editor rendering a long line) asks for more; a program
 * that uses 0.3 KiB and would otherwise reserve 8 gets its window back.
 *
 * It must be a power of two, at least one page, and no bigger than the
 * board's task-window grain (16 KiB on the C3 and the RP2040, 64 KiB on the
 * PC and the QEMU boards) - that is what the MMU-less protection units can
 * describe.  Anything else is refused at exec time, naming the number, and
 * the program does not start.
 *
 * Choose the number by measuring, not by taste: `ps` prints the high-water
 * mark in its stk column, so run the program and read what it actually used.
 */
#define TOS_STACK(bytes)                                                     \
    __attribute__((section(".note.tos"), used, aligned(4)))                  \
    static const struct {                                                    \
        unsigned namesz, descsz, type;                                       \
        char     name[8];                                                    \
        unsigned stack_bytes;                                                \
    } tos_stack_note = { 7u, 4u, 1u, "TinyOS", (unsigned)(bytes) }

/* How much heap this program wants, when what the window would leave is not
 * it.  Same note channel as TOS_STACK, a different type, so a program can
 * carry both:
 *
 *     TOS_HEAP(24 * 1024);       -- at file scope, once per program
 *
 * The kernel reads it before it sizes the window and adds it to the window
 * cost, next to the stack and the guard hole - the window is the only place
 * a task's heap can come from, since sbrk() just moves the break between
 * the image and the stack.  A script engine holding an AST, a program
 * buffering a file: these ask; a filter that reads a line at a time does
 * not need to.
 *
 * **Without this note the heap is only the slack the window happens to
 * leave after it rounds up to the grain - and for a small tool that slack is
 * usually zero** (image 4 KiB + stack 8 + guard 4 is exactly one 16 KiB
 * grain), so every malloc returns NULL with no other symptom.  A program
 * that calls malloc must declare what it needs.
 *
 * Unlike the stack, the heap has no alignment constraint of its own - the
 * break is a plain address and the PMP entry that covers it is TOR - so any
 * byte value is legal, and the only check is that the whole window still
 * fits the user area.  The request is a floor, not a ceiling: if the window
 * rounds up past it, the extra belongs to the heap (or to a stack that
 * faults into it - see process_stack_grow). */
#define TOS_HEAP(bytes)                                                      \
    __attribute__((section(".note.tos"), used, aligned(4)))                  \
    static const struct {                                                    \
        unsigned namesz, descsz, type;                                       \
        char     name[8];                                                    \
        unsigned heap_bytes;                                                 \
    } tos_heap_note = { 7u, 4u, 2u, "TinyOS", (unsigned)(bytes) }

/* How large a task window this program is allowed to need, when it must fit
 * one board grain (msh asks for one window so it stays 16 KiB on the C3 and
 * RP2040):
 *
 *     TOS_ONE_GRAIN();           -- at file scope, once per program
 *
 * The kernel ignores it; `tos build` computes image + stack + heap + guard
 * and fails the build when that no longer fits one grain (or the byte count
 * given to TOS_WINDOW), instead of letting the program quietly take a larger
 * window at run time. */
#define TOS_WINDOW(bytes)                                                    \
    __attribute__((section(".note.tos"), used, aligned(4)))                  \
    static const struct {                                                    \
        unsigned namesz, descsz, type;                                       \
        char     name[8];                                                    \
        unsigned window_bytes;                                               \
    } tos_window_note = { 7u, 4u, 3u, "TinyOS", (unsigned)(bytes) }

/* The same note with 0, meaning "one board grain" (16 KiB on C3/RP2040,
 * 64 KiB on the QEMU and PC targets). */
#define TOS_ONE_GRAIN()                                                      \
    __attribute__((section(".note.tos"), used, aligned(4)))                  \
    static const struct {                                                    \
        unsigned namesz, descsz, type;                                       \
        char     name[8];                                                    \
        unsigned window_bytes;                                               \
    } tos_window_note = { 7u, 4u, 3u, "TinyOS", 0u }

/* system calls */
int  write(int fd, const void *buf, int len);

/* write() until it is all gone.  A pipe accepts a short write when its ring
 * fills up, so anything printing more than a byte at a time has to loop or
 * it loses the rest; returns the bytes written or a negative error. */
int  write_all(int fd, const void *buf, int len);
int  read(int fd, void *buf, int len);
int  exec(const char *path, char *const argv[], int argc);
int  open(const char *path);
int  create(const char *path);
/* Remove a file.  Fails if it is a directory, a device node, or open
 * somewhere (see unlink_locked in kernel/ramdisk.c). */
int  unlink(const char *path);
int  close(int fd);
/* Configuration on an open device (GPIO_OP_* / ADC_OP_*).  Reading and
 * writing the descriptor covers values; this covers everything else. */
int  ioctl(int fd, int cmd, int arg);
/* file types returned by stat(): TOS_FTYPE_REG / _DIR / _CHR, defined in
 * tos.h so the kernel and user land cannot disagree about the numbers. */

struct stat {
    uint32_t size;   /* file size in bytes (0 for directories/devices) */
    uint8_t  type;   /* TOS_FTYPE_* */
};

int  readdir(int index, char *name, int len);
int  stat(const char *path, struct stat *st);

/* Kernel heap statistics returned by meminfo(). */
struct meminfo {
    uint32_t total;        /* heap arena size in bytes        */
    uint32_t used;         /* currently allocated             */
    uint32_t free;         /* total - used                   */
    uint32_t peak;         /* peak bytes ever allocated       */
    /* The machine, for `free`'s memory map. */
    uint32_t ram_total;    /* usable RAM from the kernel base */
    uint32_t kernel_used;  /* kernel image + .bss, heap included */
    uint32_t app_total;    /* RAM reserved for task windows   */
    uint32_t ram_unused;   /* ram_total - kernel_used - app_total */
};
int  meminfo(struct meminfo *m);

/* littlefs usage returned by dfinfo(). */
struct dfinfo {
    uint32_t total;   /* littlefs partition size in bytes */
    uint32_t used;    /* bytes consumed by littlefs        */
};
int  dfinfo(struct dfinfo *d);
int  chdir(const char *path);     /* change current working directory */
int  getcwd(char *buf, int len);  /* copy cwd into buf, returns len or -1 */
/* Switch the console to raw mode: read(0) then returns bytes as they
 * arrive (arrow keys included) and no echo is produced, so the program
 * draws the screen itself.  Returns the previous mode; 1 = raw.  The kernel
 * restores cooked mode when the process dies, so a crash cannot leave the
 * shell unable to read a line. */
int  ttyraw(int on);
/* Command history kept by the line discipline: count, and entry N counted
 * from the newest (1 = the last command run).  Returns the length or -1.
 * buf must hold at least 256 bytes, the longest line the console keeps. */
int  hist_count(void);
int  hist_get(int newest_index, char *buf, int cap);
int  getpid(void);
int  sleep(int ms);
int  uptime(void);

/* The real clock: nanoseconds since boot from a free-running hardware
 * counter, not the 10 ms scheduler tick.  The kernel hands out the same
 * instant already scaled to microseconds and milliseconds because a 32-bit
 * CPU needs a libcall for a 64-bit divide and user programs do not link
 * one: `us` and `ms` are the low 32 bits of those values (us wraps after
 * ~71 minutes, ms after ~49 days - both fine for a stopwatch, and the
 * monotonic `ns` is there when a program needs to span that). */
struct clockinfo {
    uint64_t ns;       /* monotonic nanoseconds since boot */
    uint32_t us;       /* same instant, microseconds (low 32 bits) */
    uint32_t ms;       /* same instant, milliseconds (low 32 bits) */
    uint32_t tick_hz;  /* scheduler tick rate             */
    uint32_t cpus;     /* CPUs in this build              */
};
int      clockinfo(struct clockinfo *c);
uint64_t clock_ns(void);
uint32_t clock_ms(void);
int      getcpu(void);        /* id of the CPU running this task */

/* Peripherals.  gpio_count()/adc_count() are 0 on a board that has none,
 * which is the question a program should ask before anything else. */
int gpio_count(void);
int gpio_mode(int pin, int mode);      /* TOS_PIN_* */
int gpio_write(int pin, int value);
int gpio_read(int pin);
int adc_count(void);
int adc_read(int chan);                /* raw counts, -1 on failure */
int adc_vref_mv(void);
int adc_bits(void);

/* Inter-process channels.  pipe() gives back two descriptors - read end
 * first - which behave like any other fd: read() on an empty pipe with a
 * live writer blocks, and returns 0 (EOF) once every writer has closed. */
int pipe(int fds[2]);
int dup2(int oldfd, int newfd);
int  wait(int pid);
int  yield(void);
int  ps(void);
/* 0 = highest, 2 = lowest; pid 0 means yourself.  A task cannot climb
 * above its own level, so a program cannot starve the shell. */
int  setprio(int pid, int prio);
void *sbrk(int bytes);
void exit(int code) __attribute__((noreturn));
void reboot(void) __attribute__((noreturn));

/* tiny libc */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strstr(const char *hay, const char *needle);
int    atoi(const char *s);
int    printf(const char *fmt, ...);
/* printf into a caller's buffer - the panel and file writers need the text
 * as bytes, not on stdout.  %d %u %x %c %s %%, with an optional zero/space
 * width, which is all the kernel formatter supports too. */
int    tos_vsnprintf(char *buf, int cap, const char *fmt,
                     __builtin_va_list ap);
void   puts(const char *s);
void   putchar(int c);
int    putint(int v);
int    getline(char *buf, int max);
void  *malloc(size_t n);
void   free(void *p);

/* Execute an instruction the CPU does not define.  The flat model has no
 * page faults, so this is how a program crashes on purpose: the trap kills
 * the process instead of the machine. */
#if defined(__riscv)
#define TOS_ILLEGAL_INSN() __asm__ volatile(".word 0")
#elif defined(__arm__)
#define TOS_ILLEGAL_INSN() __asm__ volatile("udf #0")
#else
#define TOS_ILLEGAL_INSN() __asm__ volatile("ud2")
#endif

#endif

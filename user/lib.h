#ifndef TINYOS_USER_LIB_H
#define TINYOS_USER_LIB_H

#include "types.h"
#include "tos.h"      /* TOS_PIN_*, the pin modes gpio_mode() takes */

/* system calls */
int  write(int fd, const void *buf, int len);
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
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    atoi(const char *s);
int    printf(const char *fmt, ...);
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

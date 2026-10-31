#include "tos.h"
#include "console.h"
#include "ramdisk.h"
#include "platform.h"
#include "process.h"
#include "elf.h"
#include "string.h"

#define LINE_MAX 256
#define MAX_EXEC_ARGS 12

static char linebuf[LINE_MAX];
static int  linelen;

static int get_char(void)
{
    for (;;) {
        int c = plat_console_getchar();
        if (c >= 0)
            return c;
        /* Busy-wait instead of sleeping: the timer only wakes us every
           10 ms, far too slow to keep up with a burst from the UART. */
        plat_cpu_idle_poll();
    }
}

static int read_line(void)
{
    linelen = 0;
    for (;;) {
        int c = get_char();
        if (c == '\r')
            c = '\n';
        if (c == '\n') {
            console_putc('\n');
            if (linelen < LINE_MAX)
                linebuf[linelen++] = '\n';
            return linelen;
        }
        if (c == 8 || c == 127) {
            if (linelen > 0) {
                linelen--;
                console_write("\b \b", 3);
            }
            continue;
        }
        if (c >= 32 && c < 127 && linelen < LINE_MAX - 2) {
            linebuf[linelen++] = (char)c;
            console_putc((char)c);
        }
    }
}

static int copy_user_string(char *dst, uint32_t src, int max)
{
    for (int i = 0; i < max - 1; i++) {
        if (!process_user_ok(src + (uint32_t)i, 1))
            return 0;
        char c = *(const char *)(src + (uint32_t)i);
        dst[i] = c;
        if (!c)
            return 1;
    }
    dst[max - 1] = '\0';
    return 1;
}

static void do_exec(regs_t *r, const syscall_t *sc)
{
    static char path[64];
    static char argbuf[MAX_EXEC_ARGS][64];
    char *argv[MAX_EXEC_ARGS];
    int argc = (int)sc->a2;

    if (argc < 0)
        argc = 0;
    if (argc > MAX_EXEC_ARGS)
        argc = MAX_EXEC_ARGS;

    if (!copy_user_string(path, sc->a0, sizeof(path))) {
        plat_frame_set_ret(r, (uint32_t)-1);
        return;
    }

    if (argc > 0) {
        if (!process_user_ok(sc->a1, (uint32_t)argc * 4)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return;
        }
        const uint32_t *uargv = (const uint32_t *)sc->a1;
        for (int i = 0; i < argc; i++) {
            argv[i] = argbuf[i];
            if (!copy_user_string(argbuf[i], uargv[i], 64)) {
                plat_frame_set_ret(r, (uint32_t)-1);
                return;
            }
        }
    }

    if (process_exec(r, path, argv, argc) != 0) {
        /* process_exec did not touch the frame on failure */
        plat_frame_set_ret(r, (uint32_t)-1);
    }
}

void syscall_dispatch(regs_t *r)
{
    syscall_t sc;
    plat_frame_get_syscall(r, &sc);

    switch (sc.nr) {
    case SYS_EXIT:
        process_kill_current(r, (int)sc.a0);
        return;

    case SYS_WRITE: {
        uint32_t fd = sc.a0, ptr = sc.a1, len = sc.a2;
        if (!process_user_ok(ptr, len)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return;
        }
        if (fd == 1 || fd == 2) {
            console_write((const char *)ptr, len);
            plat_frame_set_ret(r, len);
            return;
        }
        int n = process_fd_write((int)fd, (const void *)ptr, len);
        plat_frame_set_ret(r, (uint32_t)n);
        return;
    }

    case SYS_READ: {
        uint32_t fd = sc.a0, ptr = sc.a1, len = sc.a2;
        if (!process_user_ok(ptr, len)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return;
        }
        if (fd == 0) {
            int n = read_line();
            if ((uint32_t)n > len)
                n = (int)len;
            memcpy((void *)ptr, linebuf, (size_t)n);
            plat_frame_set_ret(r, (uint32_t)n);
            return;
        }
        int n = process_fd_read((int)fd, (void *)ptr, len);
        plat_frame_set_ret(r, (uint32_t)n);
        return;
    }

    case SYS_EXEC:
        do_exec(r, &sc);
        return;

    case SYS_OPEN: {
        char path[64];
        if (!copy_user_string(path, sc.a0, sizeof(path))) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return;
        }
        plat_frame_set_ret(r, (uint32_t)process_fd_open(path));
        return;
    }

    case SYS_CREATE: {
        char path[64];
        if (!copy_user_string(path, sc.a0, sizeof(path))) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return;
        }
        plat_frame_set_ret(r, (uint32_t)process_fd_create(path));
        return;
    }

    case SYS_CLOSE:
        process_fd_close((int)sc.a0);
        plat_frame_set_ret(r, 0);
        return;

    case SYS_READDIR: {
        uint32_t index = sc.a0, ptr = sc.a1, len = sc.a2;
        const char *name = ramdisk_name(index, NULL);
        if (!name) {
            plat_frame_set_ret(r, 0);
            return;
        }
        uint32_t n = (uint32_t)strlen(name) + 1;
        if (n > len)
            n = len;
        if (!process_user_ok(ptr, n)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return;
        }
        memcpy((void *)ptr, name, n);
        ((char *)ptr)[n - 1] = '\0';
        plat_frame_set_ret(r, n);
        return;
    }

    case SYS_GETPID:
        plat_frame_set_ret(r, (uint32_t)process_current_pid());
        return;

    case SYS_SLEEP:
        plat_sleep_ms(sc.a0);
        plat_frame_set_ret(r, 0);
        return;

    case SYS_UPTIME:
        plat_frame_set_ret(r, plat_uptime_ms());
        return;

    case SYS_SBRK:
        plat_frame_set_ret(r, process_sbrk(sc.a0));
        return;

    default:
        plat_frame_set_ret(r, (uint32_t)-1);
        return;
    }
}

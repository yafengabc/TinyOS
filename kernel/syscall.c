#include "tos.h"
#include "console.h"
#include "ramdisk.h"
#include "platform.h"
#include "process.h"
#include "tty.h"
#include "elf.h"
#include "string.h"

#define MAX_EXEC_ARGS 12

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

static regs_t *do_exec(regs_t *r, const syscall_t *sc)
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
        return r;
    }

    if (argc > 0) {
        if (!process_user_ok(sc->a1, (uint32_t)argc * 4)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        const uint32_t *uargv = (const uint32_t *)sc->a1;
        for (int i = 0; i < argc; i++) {
            argv[i] = argbuf[i];
            if (!copy_user_string(argbuf[i], uargv[i], 64)) {
                plat_frame_set_ret(r, (uint32_t)-1);
                return r;
            }
        }
    }

    plat_frame_set_ret(r, (uint32_t)process_exec(r, path, argv, argc));
    return r;
}

/* Every handler returns the frame to resume: most hand back the one that
 * came in, but a syscall that blocks gives the CPU to another task and
 * returns its frame instead. */
static regs_t *dispatch(regs_t *r);

regs_t *syscall_dispatch(regs_t *r)
{
    /* Whatever the syscall did - including waiting for a child, for the
       console or for a sleep - this is the last point at which the task's
       kernel stack is still needed, so a stop request lands here. */
    return process_suspend(dispatch(r));
}

static regs_t *dispatch(regs_t *r)
{
    syscall_t sc;
    plat_frame_get_syscall(r, &sc);

    switch (sc.nr) {
    case SYS_EXIT:
        return process_exit(r, (int)sc.a0);

    case SYS_WRITE: {
        uint32_t fd = sc.a0, ptr = sc.a1, len = sc.a2;
        if (!process_user_ok(ptr, len)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        if (fd == 1 || fd == 2) {
            console_write((const char *)ptr, len);
            plat_frame_set_ret(r, len);
            return r;
        }
        plat_frame_set_ret(r, (uint32_t)process_fd_write((int)fd, (const void *)ptr, len));
        return r;
    }

    case SYS_READ: {
        uint32_t fd = sc.a0, ptr = sc.a1, len = sc.a2;
        if (len == 0) {
            plat_frame_set_ret(r, 0);
            return r;
        }
        if (!process_user_ok(ptr, len)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        if (fd == 0) {
            /* One call per line, whatever the caller asked for: a line of
               type-ahead must not be handed over two lines' worth at a time
               (the shell's buffer would then hold two commands and echo
               them as one). */
            uint32_t room = len < TTY_LINE_MAX ? len : TTY_LINE_MAX;

            if (!tty_line_ready())
                return process_block_read(r, ptr, room);
            plat_frame_set_ret(r, (uint32_t)tty_take_line((char *)ptr, (int)room));
            return r;
        }
        plat_frame_set_ret(r, (uint32_t)process_fd_read((int)fd, (void *)ptr, len));
        return r;
    }

    case SYS_EXEC:
        return do_exec(r, &sc);

    case SYS_OPEN: {
        char path[64];
        if (!copy_user_string(path, sc.a0, sizeof(path))) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        plat_frame_set_ret(r, (uint32_t)process_fd_open(path));
        return r;
    }

    case SYS_CREATE: {
        char path[64];
        if (!copy_user_string(path, sc.a0, sizeof(path))) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        plat_frame_set_ret(r, (uint32_t)process_fd_create(path));
        return r;
    }

    case SYS_CLOSE:
        process_fd_close((int)sc.a0);
        plat_frame_set_ret(r, 0);
        return r;

    case SYS_READDIR: {
        uint32_t index = sc.a0, ptr = sc.a1, len = sc.a2;
        const char *name = ramdisk_name(index, NULL);
        if (!name) {
            plat_frame_set_ret(r, 0);
            return r;
        }
        uint32_t n = (uint32_t)strlen(name) + 1;
        if (n > len)
            n = len;
        if (!process_user_ok(ptr, n)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        memcpy((void *)ptr, name, n);
        ((char *)ptr)[n - 1] = '\0';
        plat_frame_set_ret(r, n);
        return r;
    }

    case SYS_GETPID:
        plat_frame_set_ret(r, (uint32_t)process_current_pid());
        return r;

    case SYS_SLEEP:
        return process_sleep(r, sc.a0);

    case SYS_UPTIME:
        plat_frame_set_ret(r, plat_uptime_ms());
        return r;

    case SYS_SBRK:
        plat_frame_set_ret(r, process_sbrk(sc.a0));
        return r;

    case SYS_WAIT:
        return process_wait(r, (int)sc.a0);

    case SYS_YIELD:
        return process_yield(r);

    case SYS_PS:
        plat_frame_set_ret(r, (uint32_t)process_dump());
        return r;

    case SYS_SIGNAL:
        plat_frame_set_ret(r, (uint32_t)process_signal((int)sc.a0, (int)sc.a1));
        return r;

    default:
        plat_frame_set_ret(r, (uint32_t)-1);
        return r;
    }
}

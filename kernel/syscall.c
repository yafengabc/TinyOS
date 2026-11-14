#include "tos.h"
#include "console.h"
#include "ramdisk.h"
#include "devfs.h"
#include "heap.h"
#include "platform.h"
#include "process.h"
#include "periph.h"
#include "tty.h"
#include "elf.h"
#include "string.h"
#include "mailbox.h"

#define MAX_EXEC_ARGS 12

#ifdef TOS_SYSCALL_TRACE
/* Bring-up aid for a new ARM port: a 16-entry ring of trap events, printed
 * by the fault handler when a task dies.  A syscall logs its number; the ARM
 * port logs 0x54 for a tick taken from user mode, 0x48 for a tick nested
 * inside SVCall, and 0x4600|vec for a fault.  The ordering is what separates
 * "this syscall faulted" from "a nested tick switched tasks while SVCall was
 * still active" - a distinction the register dump alone cannot make. */
uint32_t g_sys_trace[16];
uint32_t g_sys_trace_i;

void sys_trace_note(uint32_t v)
{
    g_sys_trace[g_sys_trace_i & 15u] = v;
    g_sys_trace_i++;
}
#endif

/* One argv string.  A single argument cannot be longer than the console line
 * it was typed on, so TTY_LINE_MAX is the only honest bound: at 64 a long
 * "wfile notes.txt <text...>" lost everything past the 63rd character, and
 * the program had no way to tell - the copy reported success on a string it
 * had quietly shortened. */
#define EXEC_ARG_MAX TTY_LINE_MAX

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
    /* Still no terminator: the string does not fit.  Fail instead of
       truncating - a silently shortened argument looks exactly like a
       program that lost data halfway. */
    dst[max - 1] = '\0';
    return 0;
}

/* Resolve `in` against the calling task's cwd into an absolute, normalized
 * path written to `out` (outlen bytes, always NUL-terminated).  A leading
 * '/' makes the path absolute (cwd ignored); otherwise it is joined onto the
 * cwd.  '.' and '..' are collapsed, and '..' above the root stays at '/'. */
static void resolve_path(const char *in, char *out, int outlen)
{
    char cwd[64];
    strncpy(cwd, process_cwd_str(), sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = '\0';

    const char *rest = (in && in[0]) ? in : "";
    char tmp[128];
    if (in && in[0] == '/') {
        /* Absolute: ignore the cwd. */
        int ti = 0;
        while (in[ti]) {
            if (ti < (int)sizeof(tmp) - 1)
                tmp[ti] = in[ti];
            ti++;
        }
        tmp[ti < (int)sizeof(tmp) - 1 ? ti : (int)sizeof(tmp) - 1] = '\0';
    } else {
        int bi = 0;
        while (cwd[bi] && bi < (int)sizeof(tmp) - 1)
            tmp[bi++] = cwd[bi];
        if (bi < (int)sizeof(tmp) - 1)
            tmp[bi++] = '/';
        int j = 0;
        while (rest[j] && bi < (int)sizeof(tmp) - 1)
            tmp[bi++] = rest[j++];
        tmp[bi] = '\0';
    }

    /* Normalize: drop duplicate slashes, skip '.', pop on '..', keep a
     * leading '/'. */
    char norm[128];
    int no = 0;
    int si = 0;
    while (tmp[si]) {
        while (tmp[si] == '/')
            si++;
        if (!tmp[si])
            break;
        char seg[64];
        int sl = 0;
        while (tmp[si] && tmp[si] != '/') {
            if (sl < (int)sizeof(seg) - 1)
                seg[sl++] = tmp[si];
            si++;
        }
        seg[sl] = '\0';
        if (strcmp(seg, ".") == 0)
            continue;
        if (strcmp(seg, "..") == 0) {
            if (no > 1) {
                int k = no - 1;
                while (k > 0 && norm[k] != '/')
                    k--;
                no = k;            /* keep the leading '/' */
            }
            continue;
        }
        if (no < (int)sizeof(norm) - 1)
            norm[no++] = '/';
        for (int s = 0; seg[s] && no < (int)sizeof(norm) - 1; s++)
            norm[no++] = seg[s];
    }
    if (no == 0) {
        norm[0] = '/';
        no = 1;
    }
    norm[no] = '\0';
    strncpy(out, norm, outlen - 1);
    out[outlen - 1] = '\0';
}

/* stat() for any path: device directories and device nodes first, then
 * littlefs.  One helper instead of two call sites because SYS_CHDIR asks the
 * same question ("is this a directory?") and must give the same answer - a
 * /dev you can list but not enter is not a directory.
 *
 * A device node reports size 0 and its own type: `ls` then prints "c" and
 * "<dev>" instead of the "0 bytes" a regular file would show. */
static int path_stat(const char *path, uint32_t *size, uint8_t *type)
{
    if (devfs_dir_count(path) >= 0) {
        *size = 0;
        *type = TOS_FTYPE_DIR;
        return 0;
    }
    if (devfs_lookup(path)) {
        *size = 0;
        *type = TOS_FTYPE_CHR;
        return 0;
    }
    return ramdisk_stat(path, size, type);
}

static regs_t *do_exec(regs_t *r, const syscall_t *sc)
{
    static char path[EXEC_ARG_MAX];
    char *argv[MAX_EXEC_ARGS];
    char *argbuf = NULL;            /* argv strings, from the kernel heap */
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
        /* These strings used to sit in a static 12 x 256-byte array: 3 KiB
         * of .bss held for the whole uptime to serve a window that lasts
         * one exec().  Allocate only the slots actually asked for, from the
         * same arena the loader is about to use. */
        argbuf = kmalloc((size_t)argc * EXEC_ARG_MAX);
        if (!argbuf) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        if (!process_user_ok(sc->a1, (uint32_t)argc * 4)) {
            kfree(argbuf);
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        const uint32_t *uargv = (const uint32_t *)sc->a1;
        for (int i = 0; i < argc; i++) {
            argv[i] = argbuf + (size_t)i * EXEC_ARG_MAX;
            if (!copy_user_string(argv[i], uargv[i], EXEC_ARG_MAX)) {
                kfree(argbuf);
                plat_frame_set_ret(r, (uint32_t)-1);
                return r;
            }
        }
    }

    int rc = process_exec(r, path, argv, argc);
    /* process_exec() lays the strings out in the new image's initial stack
     * before it returns, so the buffer is dead as soon as it does - on the
     * success path too, where the frame now describes the new program. */
    if (argbuf)
        kfree(argbuf);
    plat_frame_set_ret(r, (uint32_t)rc);
    return r;
}

/* Name of entry `index` in `dir`, for the two readdir syscalls.  /dev and
 * /proc are enumerated from the device table; anything else is littlefs.
 * The root is both, so its device directories ("dev", "proc") come first and
 * the littlefs files follow - that split needs both enumerators, which is
 * why it lives here and not inside either of them.
 *
 * Both enumerators copy into `name` under the filesystem lock; returning a
 * pointer into a shared buffer would let another CPU rewrite it before the
 * caller's copy. */
static int list_name_at(const char *dir, uint32_t index, char *name, int cap)
{
    int dc = devfs_dir_count(dir);

    if (dc >= 0) {
        if ((int)index < dc)
            return devfs_dir_name(dir, index, name, cap);
        if (!strcmp(dir, "/"))
            return ramdisk_list_name("/", index - (uint32_t)dc, name, cap);
        return 0;
    }
    return ramdisk_list_name(dir, index, name, cap);
}

/* Every handler returns the frame to resume: most hand back the one that
 * came in, but a syscall that blocks gives the CPU to another task and
 * returns its frame instead. */
static regs_t *dispatch(regs_t *r);

regs_t *syscall_dispatch(regs_t *r)
{
    /* No-op unless the board has no faulting protection unit (the RP2040):
     * there the guard word under the stack is the only way to notice that a
     * task ran off its bottom, and this is the first point after the fact. */
    if (!process_stack_guard(r))
        return process_suspend(process_exit(r, 139));

    /* Whatever the syscall did - including waiting for a child, for the
       console or for a sleep - this is the last point at which the task's
       kernel stack is still needed, so a stop request lands here. */
    return process_suspend(dispatch(r));
}

static regs_t *dispatch(regs_t *r)
{
    syscall_t sc;
    plat_frame_get_syscall(r, &sc);

#ifdef TOS_SYSCALL_TRACE
    sys_trace_note(sc.nr);
#endif

    switch (sc.nr) {
    case SYS_EXIT:
        return process_exit(r, (int)sc.a0);

#ifdef TOS_KTHREADS
    case SYS_THREAD_CREATE: {
        int tid = process_thread_create(sc.a0, sc.a1, sc.a2);
        plat_frame_set_ret(r, (uint32_t)tid);
        return r;
    }

    case SYS_THREAD_EXIT:
        return process_thread_exit(r, (int)sc.a0);

    case SYS_THREAD_JOIN:
        return process_thread_join(r, (int)sc.a0, sc.a1);
#endif

    case SYS_PIO: {
        struct pio_req req;
        const uint32_t up = sc.a1;
        int rc = -1;

        if (!process_user_ok(up, sizeof(req)))
            break;
        memcpy(&req, (const void *)up, sizeof(req));
        if (req.buf) {
            uint32_t bytes = (uint32_t)req.len;
            bytes *= (req.op == PIO_LOAD) ? 2u : 4u;
            if (req.len < 0 || !process_user_ok((uint32_t)req.buf, bytes))
                break;
        }
        rc = plat_pio((int)sc.a0, &req, process_current_pid());
        plat_frame_set_ret(r, (uint32_t)rc);
        return r;
    }

    case SYS_SCHEDSTAT: {
#ifdef TOS_KTHREADS
        int rc = process_schedstat(sc.a0);
#else
        int rc = -1;            /* the counters exist only with kthreads */
#endif
        plat_frame_set_ret(r, (uint32_t)rc);
        return r;
    }

    case SYS_WRITE: {
        uint32_t fd = sc.a0, ptr = sc.a1, len = sc.a2;
        if (!process_user_ok(ptr, len)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        /* Console, file or pipe: process_write decides, and may block (a
         * full pipe), which is why it hands back a frame. */
        return process_write(r, (int)fd, ptr, len);
    }

    case SYS_READ: {
        uint32_t fd = sc.a0, ptr = sc.a1, len = sc.a2;
        if (!process_user_ok(ptr, len)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        return process_read(r, (int)fd, ptr, len);
    }

    case SYS_EXEC:
        return do_exec(r, &sc);

    case SYS_OPEN: {
        char upath[64];
        if (!copy_user_string(upath, sc.a0, sizeof(upath))) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        char path[64];
        resolve_path(upath, path, sizeof(path));
        plat_frame_set_ret(r, (uint32_t)process_fd_open(path));
        return r;
    }

    case SYS_CREATE: {
        char upath[64];
        if (!copy_user_string(upath, sc.a0, sizeof(upath))) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        char path[64];
        resolve_path(upath, path, sizeof(path));
        plat_frame_set_ret(r, (uint32_t)process_fd_create(path));
        return r;
    }

    case SYS_UNLINK: {
        char upath[64];
        if (!copy_user_string(upath, sc.a0, sizeof(upath))) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        char path[64];
        resolve_path(upath, path, sizeof(path));
        /* Device nodes and the two device directories are not files.  Refuse
         * explicitly: letting littlefs answer "no such file" for something
         * `ls` just showed is the kind of error that reads as a bug. */
        if (devfs_dir_count(path) >= 0 || devfs_lookup(path)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        plat_frame_set_ret(r, (uint32_t)ramdisk_unlink(path));
        return r;
    }

    case SYS_CLOSE:
        process_fd_close((int)sc.a0);
        plat_frame_set_ret(r, 0);
        return r;

    case SYS_READDIR: {
        uint32_t index = sc.a0, ptr = sc.a1, len = sc.a2;
        char dir[64];
        char name[96];
        int n;

        strncpy(dir, process_cwd_str(), sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';

        n = list_name_at(dir, index, name, (int)sizeof(name));
        if (n <= 0) {
            plat_frame_set_ret(r, 0);
            return r;
        }
        /* A zero-length buffer is a query, not a place to write the NUL.
         * Without this the code below would validate zero bytes and then
         * write ptr[-1] in privileged mode. */
        if (len == 0) {
            plat_frame_set_ret(r, 0);
            return r;
        }
        uint32_t copy = (uint32_t)n + 1;
        if (copy > len)
            copy = len;
        if (!process_user_ok(ptr, copy)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        memcpy((void *)ptr, name, copy);
        ((char *)ptr)[copy - 1] = '\0';
        plat_frame_set_ret(r, copy);
        return r;
    }

    case SYS_READDIR_AT: {
        struct tos_dirent_req req;
        char upath[64], path[64], name[96];
        uint32_t copy;
        int n;

        if (!process_user_ok(sc.a0, sizeof(req))) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        memcpy(&req, (void *)sc.a0, sizeof(req));
        if (req.index < 0 || req.cap <= 0 ||
            !copy_user_string(upath, (uint32_t)req.path, sizeof(upath))) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        resolve_path(upath, path, sizeof(path));
        n = list_name_at(path, (uint32_t)req.index, name, (int)sizeof(name));
        if (n <= 0) {
            plat_frame_set_ret(r, 0);
            return r;
        }
        copy = (uint32_t)n + 1;
        if (copy > (uint32_t)req.cap)
            copy = (uint32_t)req.cap;
        if (!process_user_ok((uint32_t)req.name, copy)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        memcpy((void *)req.name, name, copy);
        ((char *)req.name)[copy - 1] = '\0';
        plat_frame_set_ret(r, copy);
        return r;
    }

    case SYS_POLL: {
        uint32_t ptr = sc.a0;
        int nfds = (int)sc.a1;
        struct tos_pollfd *fds = (struct tos_pollfd *)ptr;
        int ready = 0;

        if (nfds < 0 || nfds > 16 ||
            !process_user_ok(ptr, (uint32_t)nfds * sizeof(*fds))) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        for (int i = 0; i < nfds; i++) {
            short rev = process_poll(fds[i].fd, fds[i].events);

            fds[i].revents = rev;
            if (rev)
                ready++;
        }
        plat_frame_set_ret(r, (uint32_t)ready);
        return r;
    }

    case SYS_MSG_SEND: {
        uint32_t ptr = sc.a1;
        int dst = (int)sc.a0;
        int len = (int)sc.a2;

        if (len < 0 || len > TOS_MSG_MAX ||
            !process_user_ok(ptr, (uint32_t)len)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        plat_frame_set_ret(r, (uint32_t)mailbox_send(dst,
            process_current_pid(), (const void *)ptr, len));
        return r;
    }

    case SYS_MSG_RECV: {
        uint32_t ptr = sc.a0;
        int cap = (int)sc.a1;

        if (cap <= 0 || cap > TOS_MSG_MAX ||
            !process_user_ok(ptr, (uint32_t)cap)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        plat_frame_set_ret(r, (uint32_t)mailbox_recv(
            process_current_pid(), (void *)ptr, cap));
        return r;
    }

    case SYS_STAT: {
        char upath[64];
        if (!copy_user_string(upath, sc.a0, sizeof(upath))) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        char path[64];
        resolve_path(upath, path, sizeof(path));
        uint32_t ptr = sc.a1;          /* struct stat * in user space */
        uint32_t size = 0;
        uint8_t  type = 0;
        if (path_stat(path, &size, &type) < 0) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        /* struct stat { uint32_t size; uint8_t type; } is 8 bytes with
           `type` at offset 4 (uint32_t alignment). */
        if (!process_user_ok(ptr, 8)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        *(uint32_t *)ptr = size;
        *((uint8_t *)ptr + 4) = type;
        plat_frame_set_ret(r, 0);
        return r;
    }

    case SYS_MEMINFO: {
        uint32_t ptr = sc.a0;          /* struct meminfo * in user space */
        uint32_t total = heap_arena_bytes();
        uint32_t used  = heap_used_bytes();
        uint32_t free  = total - used;
        uint32_t peak  = heap_peak_bytes();
        /* The whole machine, not just the kernel heap: what the SRAM has,
         * what the kernel image takes, and what is reserved for tasks.  A
         * user asking `free` after being told the chip has 400 KiB deserves
         * the real map rather than the 32 KiB arena. */
        uint32_t ram_total   = PLAT_RAM_TOP - PLAT_RAM_BASE;
        uint32_t kernel_used = (uint32_t)__bss_end - PLAT_RAM_BASE;
        uint32_t app_total   = PLAT_APP_MAX - PLAT_APP_BASE;
        /* A board can reserve more than its SRAM claims (the mps2's app
         * range is sized for a 4 MiB part): report zero, not a wrap. */
        uint32_t ram_unused  = (kernel_used + app_total < ram_total)
                               ? ram_total - kernel_used - app_total : 0;
        if (!process_user_ok(ptr, 32)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        *(uint32_t *)ptr        = total;
        *(uint32_t *)(ptr + 4)  = used;
        *(uint32_t *)(ptr + 8)  = free;
        *(uint32_t *)(ptr + 12) = peak;
        *(uint32_t *)(ptr + 16) = ram_total;
        *(uint32_t *)(ptr + 20) = kernel_used;
        *(uint32_t *)(ptr + 24) = app_total;
        *(uint32_t *)(ptr + 28) = ram_unused;
        plat_frame_set_ret(r, 0);
        return r;
    }

    case SYS_DF: {
        uint32_t ptr = sc.a0;          /* struct dfinfo * in user space */
        uint32_t total = 0, used = 0;
        ramdisk_df(&total, &used);
        if (!process_user_ok(ptr, 8)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        *(uint32_t *)ptr       = total;
        *(uint32_t *)(ptr + 4) = used;
        plat_frame_set_ret(r, 0);
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

    case SYS_CLOCK: {
        /* struct clockinfo { u64 ns; u32 us; u32 ms; u32 tick_hz; u32 cpus; }
         * - 24 bytes.  Written as 32-bit halves because a 64-bit store on
         * RV32/ARMv6-M is not guaranteed to be a single access, and the
         * scaled values come from here because user programs do not link a
         * 64-bit division helper. */
        uint32_t ptr = sc.a0;
        if (!process_user_ok(ptr, 24)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        uint64_t ns = plat_now_ns();
        *(uint32_t *)ptr        = (uint32_t)ns;
        *(uint32_t *)(ptr + 4)  = (uint32_t)(ns >> 32);
        *(uint32_t *)(ptr + 8)  = (uint32_t)(ns / 1000ull);
        *(uint32_t *)(ptr + 12) = (uint32_t)(ns / 1000000ull);
        *(uint32_t *)(ptr + 16) = plat_tick_hz();
        *(uint32_t *)(ptr + 20) = PLAT_MAX_CPUS;
        plat_frame_set_ret(r, 0);
        return r;
    }

    case SYS_GETCPU:
        plat_frame_set_ret(r, (uint32_t)plat_cpu_id());
        return r;

    case SYS_GPIO:
        plat_frame_set_ret(r, (uint32_t)periph_gpio((int)sc.a0, (int)sc.a1,
                                                    (int)sc.a2));
        return r;

    case SYS_ADC:
        plat_frame_set_ret(r, (uint32_t)periph_adc((int)sc.a0, (int)sc.a1));
        return r;

    case SYS_PWM: {
        int op = (int)sc.a0;

        if (op == PWM_OP_SET) {
            struct tos_pwm_req req;

            if (!process_user_ok(sc.a1, sizeof(req))) {
                plat_frame_set_ret(r, (uint32_t)-1);
                return r;
            }
            memcpy(&req, (const void *)sc.a1, sizeof(req));
            plat_frame_set_ret(r, (uint32_t)periph_pwm(
                                      op, req.gpio, req.freq_hz,
                                      req.duty_permille));
        } else {
            plat_frame_set_ret(r, (uint32_t)periph_pwm(op, (int)sc.a1, 0, 0));
        }
        return r;
    }

    case SYS_UART_WRITE:
        if ((int)sc.a2 < 0 || !process_user_ok(sc.a1, sc.a2)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        plat_frame_set_ret(r, (uint32_t)periph_uart_write(
                                  (int)sc.a0, (const void *)sc.a1,
                                  (int)sc.a2));
        return r;

    case SYS_UART_READ:
        if ((int)sc.a2 < 0 || !process_user_ok(sc.a1, sc.a2)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        plat_frame_set_ret(r, (uint32_t)periph_uart_read(
                                  (int)sc.a0, (void *)sc.a1, (int)sc.a2));
        return r;

    case SYS_UART_COUNT:
        plat_frame_set_ret(r, (uint32_t)periph_uart_count());
        return r;

    case SYS_SPI: {
        int op = (int)sc.a0;

        if (op == SPI_OP_COUNT) {
            plat_frame_set_ret(r, (uint32_t)periph_spi_count());
            return r;
        }
        if (op == SPI_OP_XFER) {
            struct tos_spi_req req;

            if (!process_user_ok(sc.a1, sizeof(req))) {
                plat_frame_set_ret(r, (uint32_t)-1);
                return r;
            }
            memcpy(&req, (const void *)sc.a1, sizeof(req));
            if (req.len < 0 ||
                (req.tx && !process_user_ok((uint32_t)req.tx, req.len)) ||
                (req.rx && !process_user_ok((uint32_t)req.rx, req.len))) {
                plat_frame_set_ret(r, (uint32_t)-1);
                return r;
            }
            plat_frame_set_ret(r, (uint32_t)periph_spi_xfer(
                                      req.bus, req.cs_gpio, req.freq_hz,
                                      req.mode, req.tx, req.rx, req.len));
            return r;
        }
        plat_frame_set_ret(r, (uint32_t)-1);
        return r;
    }

    case SYS_I2C: {
        int op = (int)sc.a0;

        if (op == I2C_OP_COUNT) {
            plat_frame_set_ret(r, (uint32_t)periph_i2c_count());
            return r;
        }
        if (op == I2C_OP_XFER) {
            struct tos_i2c_req req;

            if (!process_user_ok(sc.a1, sizeof(req))) {
                plat_frame_set_ret(r, (uint32_t)-1);
                return r;
            }
            memcpy(&req, (const void *)sc.a1, sizeof(req));
            if (req.txlen < 0 || req.rxlen < 0 ||
                (req.tx && !process_user_ok((uint32_t)req.tx, req.txlen)) ||
                (req.rx && !process_user_ok((uint32_t)req.rx, req.rxlen))) {
                plat_frame_set_ret(r, (uint32_t)-1);
                return r;
            }
            plat_frame_set_ret(r, (uint32_t)periph_i2c_xfer(
                                      req.bus, req.addr, req.freq_hz,
                                      req.tx, req.txlen, req.rx, req.rxlen));
            return r;
        }
        plat_frame_set_ret(r, (uint32_t)-1);
        return r;
    }

    case SYS_DMA_MEMCPY:
        if ((int)sc.a2 < 0 ||
            !process_user_ok(sc.a0, sc.a2) ||
            !process_user_ok(sc.a1, sc.a2)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        plat_frame_set_ret(r, (uint32_t)periph_dma_memcpy(
                                  (void *)sc.a0, (const void *)sc.a1,
                                  (int)sc.a2));
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

    case SYS_SETPRIO:
        plat_frame_set_ret(r, (uint32_t)process_setprio((int)sc.a0, (int)sc.a1));
        return r;

    case SYS_REBOOT:
        /* Nothing to hand back: the machine starts over from the reset
         * vector, so this frame is abandoned along with the process table.
         * Reaching plat_reboot() from a task is fine - it drops the
         * interrupts and drains the console before touching the reset
         * register, and a reset does not care what the stack looks like. */
        kprintf("\nreboot requested by pid %d\n", process_current_pid());
        plat_reboot();

    case SYS_CHDIR: {
        char upath[64];
        if (!copy_user_string(upath, sc.a0, sizeof(upath))) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        char path[64];
        resolve_path(upath, path, sizeof(path));
        /* The root always exists and is a directory; for anything else ask
         * path_stat, so "/dev" and "/proc" are enterable exactly when they
         * are listable (same helper, same answer). */
        if (path[1] != '\0') {
            uint32_t sz = 0;
            uint8_t  ty = 0;
            if (path_stat(path, &sz, &ty) < 0 || ty != TOS_FTYPE_DIR) {
                plat_frame_set_ret(r, (uint32_t)-1);
                return r;
            }
        }
        process_set_cwd_str(path);
        plat_frame_set_ret(r, 0);
        return r;
    }

    case SYS_GETCWD: {
        uint32_t ptr = sc.a0;
        int len = (int)sc.a1;
        const char *c = process_cwd_str();
        if (len <= 0) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        uint32_t n = (uint32_t)strlen(c) + 1;
        uint32_t cp = n < (uint32_t)len ? n : (uint32_t)len;
        if (!process_user_ok(ptr, cp)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        memcpy((void *)ptr, c, cp);
        ((char *)ptr)[cp - 1] = '\0';
        plat_frame_set_ret(r, cp);
        return r;
    }

    case SYS_TTYRAW: {
        /* Return the previous mode so a program can restore it exactly
         * rather than assuming cooked. */
        int prev = tty_is_raw();
        tty_set_raw(sc.a0 != 0);
        plat_frame_set_ret(r, (uint32_t)prev);
        return r;
    }

    case SYS_HIST: {
        /* op 0: count.  op 1: copy entry sc.a1 (1 = newest) into sc.a2,
         * NUL terminated, up to the line limit.  The buffer is validated
         * for exactly the bytes we will write, not for a guessed size. */
        char tmp[TTY_LINE_MAX];
        int n;

        if (sc.a0 == 0) {
            plat_frame_set_ret(r, (uint32_t)tty_hist_count());
            return r;
        }
        n = tty_hist_get((int)sc.a1, tmp, (int)sizeof(tmp));
        if (n < 0) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        if (!process_user_ok(sc.a2, (uint32_t)n + 1)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        memcpy((void *)sc.a2, tmp, (size_t)n + 1);
        plat_frame_set_ret(r, (uint32_t)n);
        return r;
    }

    case SYS_PIPE: {
        uint32_t ptr = sc.a0;          /* int fds[2] in user space */
        int fds[2];

        if (!process_user_ok(ptr, 8)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        if (process_pipe(fds) < 0) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        ((uint32_t *)ptr)[0] = (uint32_t)fds[0];
        ((uint32_t *)ptr)[1] = (uint32_t)fds[1];
        plat_frame_set_ret(r, 0);
        return r;
    }

    case SYS_DUP2:
        plat_frame_set_ret(r, (uint32_t)process_dup2((int)sc.a0, (int)sc.a1));
        return r;

    case SYS_DUP:
        plat_frame_set_ret(r, (uint32_t)process_dup((int)sc.a0));
        return r;

    case SYS_LSEEK:
        plat_frame_set_ret(r, (uint32_t)process_lseek((int)sc.a0, (int)sc.a1,
                                                      (int)sc.a2));
        return r;

    case SYS_FSTAT: {
        uint32_t ptr = sc.a1;          /* struct stat * in user space */
        uint32_t size = 0;
        uint8_t  type = 0;

        if (process_fstat((int)sc.a0, &size, &type) < 0 ||
            !process_user_ok(ptr, 8)) {
            plat_frame_set_ret(r, (uint32_t)-1);
            return r;
        }
        *(uint32_t *)ptr = size;
        *((uint8_t *)ptr + 4) = type;
        plat_frame_set_ret(r, 0);
        return r;
    }

    case SYS_IOCTL:
        plat_frame_set_ret(r, (uint32_t)process_ioctl((int)sc.a0, (int)sc.a1,
                                                     (int)sc.a2));
        return r;

    default:
        plat_frame_set_ret(r, (uint32_t)-1);
        return r;
    }
}

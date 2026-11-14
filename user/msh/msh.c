/* msh - a shell that fits one 16 KiB task window.
 *
 * A window is image + stack + heap + guard, and the stack and guard have a
 * floor of one page each on every board with a protection unit, so the image
 * budget is 8 KiB: 16 - 4 - 4.  Measured at -Oz (clang 23.x, the toolchain
 * build.go picks for rv32/arm; zig if present) with `A | B` and `> ` in place:
 * C3 / rp2350rv / rv32 7,364 B (828 B under the 8 KiB span), rp2350 7,684 B
 * (508 B under), rp2040 8,132 B (60 B under).  All round to the 8 KiB span,
 * so the window stays 16 KiB on every 16-KiB-grain board - but rp2040 is the
 * razor's edge: one more byte over 8,192 and the span jumps to 12 KiB, doubling
 * the window to 32 KiB.  TOS_ONE_GRAIN() makes `tos build` check that rather
 * than leaving it to arithmetic in a comment.
 *
 * The pipe/redirect wiring (the `|` split, the fd dup2 dance, four error
 * strings, `> ` handling) is paid for by the POOL_MAX cut below; the span held
 * at the 8 KiB boundary rather than growing to 12 KiB.  The figures above are
 * post-pipe, post-redirect, -Oz, and they are toolchain-sensitive: an older
 * clang landed ~400 B smaller, and a future compiler bump can eat rp2040's
 * 60-byte margin.  Treat the headroom as "fits today", not "room to grow".
 *
 * What it keeps: read a line, split it, name=value, $name, ${name}, $?,
 * comments, `&`, exec a program, `A | B`, `cmd > file`, source a file
 * in-process (nests two deep), exit, run, echo, true/false, history,
 * cd/pwd/rm/ps/sleep/prio/nice.
 * What it drops: if/while/else/fi/done/break, $(...), append (`>` truncates
 * only), `|` chains longer than two stages, incr and the long help.  Those are
 * the 8 KiB of text the full shell's script engine accounts for.
 *
 * history costs no storage here: the pool lives in the line discipline
 * (kernel/tty.c, 512 B of kernel .bss), which is also what makes the arrow
 * keys work, so a shell that just calls read() gets recall for free.  The
 * `history` builtin is a reader of that pool - hist_count()/hist_get() -
 * and the 256-byte buffer it needs sits on the stack, never in .bss.
 *
 * POOL_MAX is 1.5 KiB because etc/rc is 941 bytes and etc/demo.sh is 1,140:
 * with 1 KiB the boot script still fits, a demo script does not.  The 512
 * bytes between 1.5 and 2 KiB are what the pipeline needed; nothing shipped
 * is longer than demo.sh.
 *
 * The "opt" file next to this source says -Oz, and that is what keeps msh
 * inside one 16 KiB window at all: at -O2 the same source is well over the
 * 8 KiB image budget and the window quietly becomes 32 KiB.  A shell copies
 * characters and forks programs; it has no loop anyone can measure, so the
 * size win costs nothing felt.  (The older gcc/zig numbers in
 * docs/OPEN-ITEMS section 21 are stale - clang 23 -Oz is what build.go uses
 * now.)
 *
 * Wired into init: the kernel starts bin/msh.  bin/sh is still in the image
 * and can be launched from it.  See docs/OPEN-ITEMS.md section 21.
 */
#include "lib.h"

/* The whole point: 4 KiB of stack is what makes the window one grain.  The
 * default on the small boards is 8 KiB, and 4 448 + 8 192 + 4 096 = 16 736
 * rounds up to 32 KiB on a 16 KiB grain - 352 bytes short of one window. */
TOS_STACK(4 * 1024);
/* One board grain on every board that gives us one.  The build computes
 * image + stack + heap + guard and fails when that stops fitting, so this is
 * a promise, not a hope. */
TOS_ONE_GRAIN();

#define MAX_ARGS  8
#define LINE_MAX  256
#define POOL_MAX  1536
#define MAX_LINES 32
#define MAX_VARS  4
#define VAR_NAME  12
#define VAR_VAL   64

static char line[LINE_MAX];
static char *args[MAX_ARGS];

static char pool[POOL_MAX];
static int  pool_used;

static struct { char name[VAR_NAME]; char val[VAR_VAL]; } vars[MAX_VARS];
static int nvars;
static int script_depth;
static int last_status;

static void err(const char *s)
{
    write(2, s, (int)strlen(s));
}

static int name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static char *skip_space(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static int split(char *s, char *out[], int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        out[n++] = s;
        while (*s && *s != ' ' && *s != '\t')
            s++;
        if (*s)
            *s++ = '\0';
    }
    return n;
}

static char *var_find(const char *name)
{
    for (int i = 0; i < nvars; i++)
        if (!strcmp(vars[i].name, name))
            return vars[i].val;
    return 0;
}

static void var_set(const char *name, const char *val)
{
    int i;
    for (i = 0; i < nvars; i++)
        if (!strcmp(vars[i].name, name))
            break;
    if (i == nvars) {
        if (nvars >= MAX_VARS) {
            err("sh: out of variables\n");
            return;
        }
        int n = 0;
        while (name[n] && n < VAR_NAME - 1) {
            vars[i].name[n] = name[n];
            n++;
        }
        vars[i].name[n] = '\0';
        nvars++;
    }
    int n = 0;
    if (val)
        while (val[n] && n < VAR_VAL - 1) {
            vars[i].val[n] = val[n];
            n++;
        }
    vars[i].val[n] = '\0';
}

/* $name, ${name}, $?.  No quoting, no capture: every extra feature costs
 * bytes the window does not have. */
static void expand(const char *in, char *out, int cap)
{
    int n = 0;
    while (*in && n < cap - 1) {
        if (*in != '$') {
            out[n++] = *in++;
            continue;
        }
        in++;
        if (*in == '?') {
            out[n++] = (char)('0' + (last_status & 7));
            in++;
            continue;
        }
        char name[VAR_NAME];
        int k = 0;
        if (*in == '{') {
            in++;
            while (*in && *in != '}' && k < VAR_NAME - 1)
                name[k++] = *in++;
            if (*in == '}')
                in++;
        } else {
            while (name_char(*in) && k < VAR_NAME - 1)
                name[k++] = *in++;
        }
        name[k] = '\0';
        if (!k) {
            out[n++] = '$';
            continue;
        }
        char *v = var_find(name);
        if (v)
            while (*v && n < cap - 1)
                out[n++] = *v++;
    }
    out[n] = '\0';
}

static int spawn(char *argv[], int argc)
{
    char path[64];
    if (strchr(argv[0], '/')) {
        if (strlen(argv[0]) >= sizeof(path))
            return -1;
        strcpy(path, argv[0]);
    } else {
        if (strlen(argv[0]) + 6 >= sizeof(path))
            return -1;
        strcpy(path, "/bin/");
        strcpy(path + 5, argv[0]);
    }
    return exec(path, argv, argc);
}

static int run_script(const char *path);
static int run_command(char *s);

/* The target of the `>` this command line carries, while it is in force,
 * else NULL.  run_pipeline() needs it: in `A | B > f` the file belongs on the
 * right-hand stage only, so it is connected after the pipe's own fd 1 has been
 * closed and before that stage is spawned. */
static char *redirect_target;

/* Open `target` for writing, saying why if it cannot be done.  create()
 * first, open() second: `>` truncates a regular file, while a device node
 * refuses create() and opens fine on the retry. */
static int open_for_write(char *target)
{
    int fd = create(target);

    if (fd < 0)
        fd = open(target);
    if (fd < 0) {
        err("sh: cannot write to ");
        err(target);
        err("\n");
        last_status = 1;
    }
    return fd;
}

/* A | B, external programs only: a child cannot be wired up between exec()
 * and its first instruction, so the shell moves its own fd 1/0 onto the pipe
 * ends around each spawn.  Both halves of the pipe have to be closed in the
 * shell afterwards - EOF means *every* write end is gone, and a shell still
 * holding one would leave B waiting forever. */
static void run_pipeline(char *left, char *right)
{
    char *largv[MAX_ARGS], *rargv[MAX_ARGS];
    int lac, rac, fds[2], lpid, rpid;

    /* Two stages, not a chain: a third would otherwise arrive as an
     * ordinary argument and be reported as a missing file.  Checked before
     * split(), which turns the spaces into NULs and would hide it. */
    if (strchr(right, '|')) {
        err("sh: only two stages per pipeline\n");
        last_status = 1;
        return;
    }
    lac = split(left, largv, MAX_ARGS);
    rac = split(right, rargv, MAX_ARGS);
    if (!lac || !rac) {
        err("sh: empty command in a pipeline\n");
        last_status = 1;
        return;
    }
    if (pipe(fds) < 0) {
        err("sh: no descriptors left for a pipe\n");
        last_status = 1;
        return;
    }

    if (dup2(fds[1], 1) < 0) {
        err("sh: cannot connect the pipe to stdout\n");
        close(fds[0]);
        close(fds[1]);
        last_status = 1;
        return;
    }
    lpid = spawn(largv, lac);
    close(1);                          /* back to the console on our side */
    close(fds[1]);
    if (lpid < 0) {
        /* No writer ever existed, so the right side would never see EOF. */
        close(fds[0]);
        last_status = 127;
        return;
    }

    if (dup2(fds[0], 0) < 0) {
        err("sh: cannot connect the pipe to stdin\n");
        close(fds[0]);
        wait(lpid);
        last_status = 1;
        return;
    }

    /* `A | B > f`: the last stage is the one whose stdout is the file, and it
     * inherits fd 1 from here.  The handle that create() returned is closed
     * before the spawn - it is the one that truncated the file, and its
     * metadata goes back to disk with a size of zero, which would undo every
     * byte the child writes if it were still open when they land. */
    if (redirect_target) {
        int ofd = open_for_write(redirect_target);

        if (ofd < 0) {
            close(fds[0]);
            wait(lpid);
            return;
        }
        dup2(ofd, 1);
        close(ofd);
    }
    rpid = spawn(rargv, rac);
    if (redirect_target)
        close(1);                      /* back to the console */
    close(0);
    close(fds[0]);

    /* Wait in the order they were started: the left one finishing is what
     * gives the right one its EOF. */
    wait(lpid);
    last_status = rpid < 0 ? 127 : (wait(rpid) < 0 ? 127 : 0);
}

/* `cmd > file` and `cmd > /dev/gpio4`.  The left side goes back through
 * run_command(), so `A | B > file` reaches run_pipeline() with the target
 * still set and puts the file on the last stage only. */
static int run_redirect(char *cmd, char *target)
{
    char *outer = redirect_target;
    int fd, rc;

    target = skip_space(target);
    {   /* A trailing space would otherwise become part of the name. */
        int n = (int)strlen(target);

        while (n > 0 && (target[n - 1] == ' ' || target[n - 1] == '\t'))
            target[--n] = '\0';
    }
    if (!*target) {
        err("sh: nothing after '>'\n");
        last_status = 1;
        return -1;
    }
    fd = open_for_write(target);
    if (fd < 0)
        return -1;
    if (dup2(fd, 1) < 0) {
        close(fd);
        err("sh: cannot redirect stdout\n");
        last_status = 1;
        return -1;
    }
    /* Closed before the command runs: this is the handle whose truncation is
     * still pending, and closing it afterwards would write a size of zero
     * over everything the command just put in the file. */
    close(fd);

    redirect_target = target;
    rc = run_command(cmd);
    redirect_target = outer;
    close(1);                          /* back to the console */
    return rc;
}

static int run_command(char *s)
{
    char *p = skip_space(s);
    if (!*p || *p == '#')
        return -1;

    /* name=value, before anything else looks for a command. */
    {
        char *q = p;
        while (*q && *q != ' ' && *q != '\t' && *q != '=') {
            if (!name_char(*q))
                break;
            q++;
        }
        if (*q == '=' && q > p && (q - p) < VAR_NAME) {
            *q = '\0';
            var_set(p, skip_space(q + 1));
            last_status = 0;
            return -1;
        }
    }

    /* A redirect is split off first: what is left may still be a pipeline,
     * and `a | b > f` should send the whole pipeline to the file. */
    {
        char *gt = p;
        while (*gt && *gt != '>')
            gt++;
        if (*gt == '>') {
            if (gt[1] == '>') {
                err("sh: no append, '>' truncates\n");
                last_status = 1;
                return -1;
            }
            *gt = '\0';
            return run_redirect(p, gt + 1);
        }
    }

    /* A pipeline is split before tokenising: '|' separates two command
     * lines, and each half keeps its own arguments.  Builtins do not take
     * part - both halves are programs in /bin. */
    {
        char *bar = p;
        while (*bar && *bar != '|')
            bar++;
        if (*bar == '|') {
            *bar = '\0';
            run_pipeline(p, bar + 1);
            return -1;
        }
    }

    int ac = split(s, args, MAX_ARGS);
    if (ac == 0)
        return -1;

    int bg = 0;
    if (ac > 1 && !strcmp(args[ac - 1], "&")) {
        bg = 1;
        ac--;
    }

    if (!strcmp(args[0], "exit")) {
        if (!script_depth) {
            puts("bye.\n");
            exit(0);
        }
        return ac > 1 ? atoi(args[1]) & 255 : 0;
    }
    if (!strcmp(args[0], "source")) {
        char name[64];
        if (ac < 2) {
            err("usage: source <file>\n");
            last_status = 1;
        } else if (strlen(args[1]) >= sizeof(name)) {
            err("sh: name too long\n");
            last_status = 1;
        } else {
            strcpy(name, args[1]);
            int r = run_script(name);
            if (r < 0) {
                if (r == -1) {
                    err("sh: cannot open ");
                    err(name);
                    err("\n");
                }
                last_status = 1;
            } else {
                last_status = r;
            }
        }
        return -1;
    }
    if (!strcmp(args[0], "run")) {
        if (ac < 2) {
            err("usage: run <program> [args...]\n");
            last_status = 1;
        } else {
            int pid = spawn(&args[1], ac - 1);
            if (pid < 0)
                last_status = 127;
            else if (!bg)
                last_status = wait(pid) < 0 ? 127 : 0;
            else
                last_status = 0;
        }
        return -1;
    }
    if (!strcmp(args[0], "echo") && !bg) {
        for (int i = 1; i < ac; i++) {
            if (i > 1)
                putchar(' ');
            puts(args[i]);
        }
        putchar('\n');
        last_status = 0;
        return -1;
    }
    if (!strcmp(args[0], "history")) {
        /* The list is the kernel's, the same pool the arrow keys walk:
         * hist_count()/hist_get() are two syscalls against tty.c's 512
         * bytes, so this builtin adds no static storage at all.  The copy
         * buffer lives on the stack - the window already paid for 4 KiB of
         * it and a nested source only got 1.6 KiB deep. */
        char h[256];
        int n = hist_count();

        for (int i = n; i >= 1; i--) {
            if (hist_get(i, h, (int)sizeof(h)) < 0)
                continue;
            /* "printf(\"%4d  %s\n\")" without printf: the full shell's
             * version drags in the formatter, which is most of a kilobyte
             * of text this window does not have. */
            int k = n - i + 1;
            for (int w = k < 10 ? 3 : k < 100 ? 2 : 1; w > 0; w--)
                putchar(' ');
            putint(k);
            puts("  ");
            puts(h);
            putchar('\n');
        }
        last_status = 0;
        return -1;
    }
    if (!strcmp(args[0], "true")) {
        last_status = 0;
        return -1;
    }
    if (!strcmp(args[0], "false")) {
        last_status = 1;
        return -1;
    }
    if (!strcmp(args[0], "cd")) {
        const char *dst = ac > 1 ? args[1] : "/";

        if (chdir(dst) < 0) {
            err("cd: no such directory: ");
            err(dst);
            err("\n");
            last_status = 1;
        } else {
            last_status = 0;
        }
        return -1;
    }
    if (!strcmp(args[0], "pwd")) {
        char cwd[64];

        if (getcwd(cwd, sizeof(cwd)) > 0) {
            puts(cwd);
            putchar('\n');
        }
        last_status = 0;
        return -1;
    }
    if (!strcmp(args[0], "rm")) {
        if (ac < 2) {
            err("usage: rm <file>...\n");
            last_status = 1;
        } else {
            last_status = 0;
            for (int i = 1; i < ac; i++)
                if (unlink(args[i]) < 0) {
                    err("rm: cannot remove ");
                    err(args[i]);
                    err("\n");
                    last_status = 1;
                }
        }
        return -1;
    }
    if (!strcmp(args[0], "ps")) {
        /* Through /proc/ps, so the output could be redirected if msh ever
         * grows redirection; the syscall path writes to the console only. */
        int fd = open("/proc/ps");

        if (fd < 0) {
            err("ps: /proc/ps not available\n");
            last_status = 1;
        } else {
            char buf[128];
            int n;

            while ((n = read(fd, buf, sizeof(buf))) > 0)
                write_all(1, buf, n);
            close(fd);
            last_status = 0;
        }
        return -1;
    }
    if (!strcmp(args[0], "sleep") && !bg) {
        int ms = ac > 1 ? atoi(args[1]) : 0;

        if (ac < 2 || ms <= 0 || ms > 3600000) {
            err("usage: sleep <ms>   (max 3600000)\n");
            last_status = 1;
        } else {
            sleep(ms);
            last_status = 0;
        }
        return -1;
    }
    if (!strcmp(args[0], "prio")) {
        if (ac != 3) {
            err("usage: prio <pid> <0-2>  (0 highest, 2 lowest)\n");
            last_status = 1;
        } else if (setprio(atoi(args[1]), atoi(args[2])) < 0) {
            err("prio: no such task, or a level you cannot set\n");
            last_status = 1;
        } else {
            last_status = 0;
        }
        return -1;
    }
    if (!strcmp(args[0], "nice")) {
        int lvl = ac > 1 ? atoi(args[1]) : -1;

        if (ac < 3 || lvl < 0 || lvl > 2) {
            err("usage: nice <0-2> <program> [args...] [&]\n");
            last_status = 1;
        } else {
            int pid = spawn(&args[2], ac - 2);

            if (pid < 0) {
                last_status = 127;
            } else {
                if (setprio(pid, lvl) < 0)
                    err("nice: cannot move that task to that level\n");
                if (bg) {
                    puts("[");
                    putint(pid);
                    puts("]\n");
                    last_status = 0;
                } else {
                    int rc = wait(pid);

                    last_status = rc < 0 ? 127 : rc;
                }
            }
        }
        return -1;
    }

    int pid = spawn(args, ac);
    if (pid < 0) {
        last_status = 127;
        return -1;
    }
    if (bg) {
        puts("[");
        putint(pid);
        puts("]\n");
        last_status = 0;
        return -1;
    }
    int rc = wait(pid);
    last_status = rc < 0 ? 127 : rc;
    return -1;
}

static int load_script(const char *path, char *lines[], int maxlines, int *mark)
{
    int fd = open(path);
    if (fd < 0)
        return -1;

    *mark = pool_used;
    int room = POOL_MAX - pool_used - 2;
    if (room <= 0) {
        close(fd);
        err("sh: script text pool full\n");
        return -2;
    }
    char *base = pool + pool_used;
    int total = 0;
    while (total < room) {
        int r = read(fd, base + total, room - total);
        if (r <= 0)
            break;
        total += r;
    }
    if (total == room) {
        char extra;
        if (read(fd, &extra, 1) > 0) {
            close(fd);
            err("sh: script too long\n");
            pool_used = *mark;
            return -2;
        }
    }
    close(fd);
    base[total] = '\0';
    pool_used += total + 1;

    int n = 0;
    char *p = base;
    while (p < base + total) {
        char *s = p;
        while (p < base + total && *p != '\n' && *p != '\r')
            p++;
        if (p < base + total) {
            *p++ = '\0';
            if (p < base + total && (*p == '\n' || *p == '\r'))
                *p++ = '\0';
        }
        char *q = skip_space(s);
        if (!*q || *q == '#')
            continue;
        if (n >= maxlines) {
            err("sh: too many script lines\n");
            pool_used = *mark;
            return -2;
        }
        lines[n++] = s;
    }
    return n;
}

static int run_script(const char *path)
{
    char *lines[MAX_LINES];
    int mark;
    int n;

    if (script_depth >= 2) {
        err("sh: source nested too deep\n");
        return -2;
    }
    n = load_script(path, lines, MAX_LINES, &mark);
    if (n < 0)
        return n;

    script_depth++;
    int code = 0;              /* a script of only comments succeeds */
    for (int i = 0; i < n; i++) {
        char cmd[LINE_MAX];
        expand(lines[i], cmd, sizeof(cmd));
        int r = run_command(cmd);
        if (r >= 0) {
            code = r;
            break;
        }
    }
    script_depth--;
    pool_used = mark;
    return code;
}

int main(int argc, char *argv[])
{
    if (argc > 1) {
        int r = run_script(argv[1]);
        if (r < 0) {
            if (r == -1) {
                err("sh: cannot open ");
                err(argv[1]);
                err("\n");
            }
            return 2;
        }
        return r;
    }

    /* One short line: the LCD console is 40 columns and wraps the rest. */
    puts("\nTinyOS shell (pid ");
    putint(getpid());
    puts(").\n");

    if (getpid() == 1)
        run_script("etc/rc");

    for (;;) {
        puts("tos> ");
        if (getline(line, sizeof(line)) < 0)
            continue;
        char cmd[LINE_MAX];
        expand(line, cmd, sizeof(cmd));
        run_command(cmd);
    }
}

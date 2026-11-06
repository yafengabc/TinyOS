/* msh - a shell that fits one 16 KiB task window.
 *
 * A window is image + stack + guard, and the last two have a floor of one
 * page each on every board with a protection unit, so the image budget is
 * 8 KiB: 16 - 4 - 4.  Measured, on C3: 6 272 B of image (2 952 text, 331
 * rodata, 2 656 bss) and `ps` reports wink 16 / stk 1.6-of-4 after a
 * nested `source`.  The full shell needs 20 464 + 12 288 = 32 752, i.e. a
 * 32 KiB window with 16 bytes to spare.
 *
 * What it keeps: read a line, split it, name=value, $name, ${name}, $?,
 * comments, `&`, exec a program, source a file in-process (nests two
 * deep), exit, run, echo, true/false, history.
 * What it drops: if/while/else/fi/done/break, $(...), pipes, redirection,
 * incr, cd/pwd/rm/ps/prio/nice, the sleep builtin, the long help.
 * Those are the 8 KiB of text the full shell's script engine and builtins
 * account for.
 *
 * history costs no storage here: the pool lives in the line discipline
 * (kernel/tty.c, 512 B of kernel .bss), which is also what makes the arrow
 * keys work, so a shell that just calls read() gets recall for free.  The
 * `history` builtin is a reader of that pool - hist_count()/hist_get() -
 * and the 256-byte buffer it needs sits on the stack, never in .bss.
 *
 * POOL_MAX is 2 KiB because etc/rc is 941 bytes and etc/demo.sh is 1 140:
 * with 1 KiB the boot script still fits, a demo script does not.  On
 * rp2040 the image grows to 7 968 B (Thumb plus the aeabi division
 * helpers), so only 224 bytes of the budget are left there.
 *
 * Not wired into init: the kernel still starts bin/sh, and this is a
 * second program in the image.  See docs/OPEN-ITEMS.md section 21.
 */
#include "lib.h"

/* The whole point: 4 KiB of stack is what makes the window one grain.  The
 * default on the small boards is 8 KiB, and 4 448 + 8 192 + 4 096 = 16 736
 * rounds up to 32 KiB on a 16 KiB grain - 352 bytes short of one window. */
TOS_STACK(4 * 1024);

#define MAX_ARGS  8
#define LINE_MAX  256
#define POOL_MAX  2048
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

    puts("\nTinyOS shell (pid ");
    putint(getpid());
    puts("). Arrows recall, 'history' lists.\n");

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

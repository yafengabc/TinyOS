#include "lib.h"

#define MAX_ARGS   8
#define LINE_MAX   256
/* `source` nests in this process, so the only real limit is the stack. */
#define MAX_SOURCE 4
/* Script text lives in one pool.  A `source` appends its file after the
 * caller's and gives the bytes back on return, so a `source` inside a loop
 * does not grow anything - the reason for not using malloc(), whose bump
 * allocator has no free().  Only MAX_SOURCE layers are ever alive at once. */
/* 3 KiB, and the number is not free: it sets the shell's window.  The window
 * is image + stack + guard rounded up to the 16 KiB grain, so sh lands in
 * 32 KiB only while its image stays within 20 KiB (20 + 8 + 4 = 32, and 21 KiB
 * of image would push it to 48).  Dropping the pool from 4 KiB to 3 was the
 * cheapest way to cross that line: cutting the stack instead (TOS_STACK(4K))
 * buys the same 32 KiB but leaves no room for the stack to grow into, and a
 * nested `source` promptly faulted at 4 KiB (measured 2026-11-06). */
#define POOL_MAX   3072
#define MAX_LINES  64
#define MAX_BLOCKS 8
#define MAX_VARS   8
#define VAR_NAME   12
#define VAR_VAL    96
/* A `while` whose condition never changes would own the shell forever, and
 * unlike a program there is no task to ^C.  Cut it off with a message. */
#define MAX_ITER   10000

/* One parser, two sources: the console hands over one line at a time, a
 * script is sliced into lines from the pool.  Both go through expand() and
 * run_command(), so a variable works the same way typed and in a file. */
static char line[LINE_MAX];
static char *args[MAX_ARGS];

static char pool[POOL_MAX];
static int  pool_used;

struct var {
    char name[VAR_NAME];
    char val[VAR_VAL];
};
static struct var vars[MAX_VARS];
static int nvars;

static int script_depth;
static int last_status;         /* $? and the exit code of a script's `exit` */

/* ---- small helpers ---------------------------------------------------- */

static void sh_err(const char *s)
{
    /* fd 2, not puts(): an error about a redirection or a capture must not
     * land in the file (or the pipe) fd 1 was just pointed at. */
    write(2, s, (int)strlen(s));
}

/* No snprintf in the tiny libc that user programs link, and putint() writes
 * straight to the console.  Conditions and `incr` need an integer as text. */
static void str_num(char *buf, int v)
{
    char tmp[12];
    int n = 0, i = 0;
    unsigned int u;

    if (v < 0) {
        buf[i++] = '-';
        u = 0u - (unsigned int)v;   /* no -INT_MIN overflow on RV32 either */
    } else {
        u = (unsigned int)v;
    }
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (n)
        buf[i++] = tmp[--n];
    buf[i] = '\0';
}

static int name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
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
    if (!val)
        val = "";
    for (int i = 0; i < nvars; i++) {
        if (!strcmp(vars[i].name, name)) {
            int n = 0;
            while (val[n] && n < VAR_VAL - 1) {
                vars[i].val[n] = val[n];
                n++;
            }
            vars[i].val[n] = '\0';
            return;
        }
    }
    if (nvars >= MAX_VARS) {
        sh_err("sh: out of variables\n");
        return;
    }
    int n = 0;
    while (name[n] && n < VAR_NAME - 1) {
        vars[nvars].name[n] = name[n];
        n++;
    }
    vars[nvars].name[n] = '\0';
    n = 0;
    while (val[n] && n < VAR_VAL - 1) {
        vars[nvars].val[n] = val[n];
        n++;
    }
    vars[nvars].val[n] = '\0';
    nvars++;
}

/* True when the first word of `s` is exactly `w`.  Never writes to `s`:
 * scan_block() uses it to look ahead at lines that may never be executed. */
static int word_eq(const char *s, const char *w)
{
    while (*s == ' ' || *s == '\t')
        s++;
    while (*w) {
        if (*s != *w)
            return 0;
        s++;
        w++;
    }
    return *s == '\0' || *s == ' ' || *s == '\t';
}

static char *skip_word(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    while (*s && *s != ' ' && *s != '\t')
        s++;
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
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

static void app_str(char *out, int *n, int cap, const char *s)
{
    while (*s && *n < cap - 1)
        out[(*n)++] = *s++;
}

static void cannot_open(const char *path)
{
    puts("sh: cannot open ");
    puts(path);
    puts("\n");
}

/* ---- command help ----------------------------------------------------- */

static void cmd_help(void)
{
    puts("TinyOS shell commands:\n");
    puts("  Line editing: ^A/^E home/end, ^U kill line, ^K kill to end,\n");
    puts("  ^W kill word, arrows move/history, Backspace, Delete, Tab=space.\n");
    puts("  history            list recent commands (up/down recalls them)\n");
    puts("  ls [dir|file]      list a directory, or show a file\n");
    puts("  cd [dir]           change directory (default /)\n");
    puts("  pwd                print the working directory\n");
    puts("  cat <file>         print a file\n");
    puts("  edit <file>        full screen editor (^S save, ^X exit)\n");
    puts("  echo [args...]     print arguments (builtin)\n");
    puts("  hello              demo program\n");
    puts("  mem                heap / sbrk demo\n");
    puts("  crash              run an illegal instruction (kills that process)\n");
    puts("  uptime             real time since boot (hardware clock)\n");
    puts("  sleep <ms>         wait without using the CPU (builtin in scripts)\n");
    puts("  gpio <pin> [0|1]   read or drive a pin (gpio blink <pin>)\n");
    puts("  screen             draw a status card on the board's panel\n");
    puts("  screen dev         the device table, on the panel\n");
    puts("  screen text <s>    write text; screen mirror on|off\n");
    puts("  splash             ASCII title card; 'splash off' restores it\n");
    puts("  lcdconsole on|off  toggle the panel's copy of console output\n");
    puts("  fb [demo|bars|info|clear|rect]  draw RGB565 pixels on /dev/fb0\n");
    puts("  adc [chan] [n]     sample the analog inputs\n");
    puts("  smp                run one worker per CPU, in parallel\n");
    puts("  reboot             reset the machine and boot again\n");
    puts("  df                 filesystem usage\n");
    puts("  dd of=f bs=n count=n  write n blocks of a test pattern; with\n");
    puts("                     if=f instead of of= read back and verify\n");
    puts("  rm <file>...       remove files\n");
    puts("  free               RAM map and kernel heap usage\n");
    puts("  ps                 list tasks\n");
    puts("  cat /proc/ps       the same table as a file (also /proc/mem,\n");
    puts("                     /proc/uptime) - `ps > f` writes it to f\n");
    puts("  echo 1 > /dev/gpio4  pins and ADC channels are files: write to\n");
    puts("                     drive, cat to read, `gpio dev` for ioctl\n");
    puts("  cmd > file         send stdout to a file or a device\n");
    puts("  A | B              send A's stdout to B's stdin\n");
    puts("  run <prog> [args]  run a program from /bin\n");
    puts("  run <prog> ... &   run it in the background\n");
    puts("  run sh <file>      run a script as a task\n");
    puts("  source <file>      run a script in this shell\n");
    puts("  nice <n> <prog> .. run it at priority n (0 high, 2 low)\n");
    puts("  prio <pid> <n>     move a running task to priority n\n");
    puts("Script syntax (in a script file; see docs/SHELL-GUIDE.md):\n");
    puts("  name=text          set a variable (also `name=$(prog args)`)\n");
    puts("  incr name [n]      add n (default 1) to an integer variable\n");
    puts("  $name  ${name}  $? text substitution; $(prog) captures stdout\n");
    puts("  if <a> == <b>     also != -eq -ne -lt -gt -le -ge, -n a, -z a\n");
    puts("  else / fi         optional else branch / end of if\n");
    puts("  while <cond>      loop while the condition holds\n");
    puts("  done / break      end of loop / leave the innermost loop\n");
    puts("  true / false      set $? to 0 / 1\n");
    puts("  help               this text\n");
    puts("  exit [code]        shut TinyOS down (end the script, in a script)\n");
}

/* The prompt shows the working directory when it is not the root, so a `cd`
 * is visible; it always ends in "tos> " so scripts and the test harness can
 * still recognise the prompt. */
static void print_prompt(void)
{
    char buf[64];
    if (getcwd(buf, sizeof(buf)) > 0 && strcmp(buf, "/") != 0) {
        puts(buf);
        puts(" tos> ");
    } else {
        puts("tos> ");
    }
}

/* ---- spawning programs ------------------------------------------------ */

/* exec only spawns a child; the caller decides whether to wait for it. */
static int spawn(char *argv[], int argc)
{
    char path[64];

    strcpy(path, "bin/");
    if (strlen(path) + strlen(argv[0]) >= sizeof(path)) {
        puts("sh: name too long\n");
        return -1;
    }
    strcpy(path + 4, argv[0]);
    return exec(path, argv, argc);
}

static int run_program(char *argv[], int argc, int bg)
{
    int pid = spawn(argv, argc);
    if (pid < 0) {
        last_status = 127;         /* the kernel already said why */
        return pid;
    }
    if (bg) {
        puts("[");
        putint(pid);
        puts("]\n");
        last_status = 0;
        return 0;
    }
    int rc = wait(pid);
    last_status = rc < 0 ? 127 : rc;
    return rc;
}

/* Start a program at a lower (or equal) priority level.  exec only spawns,
 * so the child is still READY and has never run when its level is set -
 * there is no window in which it takes a slice at the wrong level. */
static void run_nice(char *argv[], int argc, int bg, int lvl)
{
    int pid = spawn(argv, argc);
    if (pid < 0)
        return;
    if (setprio(pid, lvl) < 0)
        puts("nice: cannot move that task to that level\n");

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

/* A | B: run two programs with a pipe between them.
 *
 * The shell does the wiring because a child cannot run code between exec()
 * and its first instruction.  It also has to close both of its own copies
 * afterwards: a pipe's EOF means "every write end is gone", and the shell
 * holding one would keep `B` waiting forever. */
static void run_pipeline(char *left, char *right)
{
    char *largv[MAX_ARGS], *rargv[MAX_ARGS];
    int lac, rac, fds[2], lpid = -1, rpid = -1;

    lac = split(left, largv, MAX_ARGS);
    rac = split(right, rargv, MAX_ARGS);
    if (lac == 0 || rac == 0) {
        puts("sh: empty command in a pipeline\n");
        return;
    }

    if (pipe(fds) < 0) {
        puts("sh: no descriptors left for a pipe\n");
        return;
    }

    if (dup2(fds[1], 1) < 0) {
        puts("sh: cannot connect the pipe to stdout\n");
        close(fds[0]);
        close(fds[1]);
        return;
    }
    lpid = spawn(largv, lac);
    close(1);                   /* restore the console on our side */
    close(fds[1]);

    if (lpid < 0) {
        /* Left side never started: the read end has no writer, so nobody
         * would ever send EOF to the right side.  Drop it all. */
        close(fds[0]);
        last_status = 127;
        return;
    }

    if (dup2(fds[0], 0) < 0) {
        puts("sh: cannot connect the pipe to stdin\n");
        close(fds[0]);
        wait(lpid);
        return;
    }
    rpid = spawn(rargv, rac);
    close(0);
    close(fds[0]);

    /* Wait for both, in the order they were started: the left one finishing
     * is what gives the right one its EOF. */
    wait(lpid);
    if (rpid >= 0) {
        int rc = wait(rpid);
        last_status = rc < 0 ? 127 : rc;
    }
}

static int run_command(char *s);
static int run_script(const char *path);

/* `cmd > file` and `cmd > /dev/gpio4`.
 *
 * create() first, open() second.  `>` means truncate for a regular file; a
 * device node refuses create() and opens fine on the retry. */
static int run_redirect(char *cmd, char *target)
{
    int fd, rc;

    target = skip_space(target);
    if (!*target) {
        sh_err("sh: nothing after '>'\n");
        return -1;
    }

    fd = create(target);
    if (fd < 0)
        fd = open(target);
    if (fd < 0) {
        sh_err("sh: cannot write to ");
        sh_err(target);
        sh_err("\n");
        return -1;
    }
    if (dup2(fd, 1) < 0) {
        close(fd);
        sh_err("sh: cannot redirect stdout\n");
        return -1;
    }
    close(fd);

    rc = run_command(cmd);
    close(1);                   /* back to the console */
    return rc;
}

/* ---- substitution ----------------------------------------------------- */

static int expand(const char *in, char *out, int cap, int allow_capture);

/* `$(prog args)` is the shell's own pipe, not `A | B`: fd 1 is pointed at
 * the write end, the program is spawned, and this shell reads the read end
 * to EOF into the substitution text.  Reading *before* wait() is not an
 * optimisation - the pipe holds 2048 bytes and a producer that fills it
 * blocks, so a shell that waited first would deadlock with its own child.
 *
 * Everything printed on this path goes to fd 2: fd 1 is the pipe, and a
 * `puts()` here would be mistaken for the program's output. */
static int capture_program(char *av[], int ac, char *out, int cap)
{
    int fds[2], n = 0, overflow = 0, pid = -1;

    if (pipe(fds) < 0) {
        sh_err("sh: no descriptors left for $(...)\n");
        last_status = 127;
        goto done;
    }
    if (dup2(fds[1], 1) < 0) {
        sh_err("sh: cannot connect $(...) to stdout\n");
        close(fds[0]);
        close(fds[1]);
        last_status = 127;
        goto done;
    }
    if (strlen(av[0]) >= 60) {  /* spawn() prints to fd 1 on this one */
        sh_err("sh: program name too long in $(...)\n");
        close(1);
        close(fds[0]);
        close(fds[1]);
        last_status = 127;
        goto done;
    }
    pid = spawn(av, ac);
    close(1);                   /* restore our stdout, drop our writer */
    close(fds[1]);

    /* From here only the child holds a write end, so EOF means it is gone. */
    {
        char buf[64];
        int r;
        while ((r = read(fds[0], buf, sizeof(buf))) > 0) {
            for (int i = 0; i < r; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r')
                    c = ' ';
                if (n < cap - 1)
                    out[n++] = c;
                else
                    overflow = 1;   /* keep draining, or the child blocks */
            }
        }
    }
    close(fds[0]);
    if (pid >= 0) {
        int rc = wait(pid);
        last_status = rc < 0 ? 127 : rc;
    } else {
        last_status = 127;
    }
    if (overflow)
        sh_err("sh: $(...) output truncated\n");

done:
    out[n] = '\0';
    while (n > 0 && out[n - 1] == ' ')
        out[--n] = '\0';
    return n;
}

/* The command inside `$(...)` may itself mention variables.  `subst` is a
 * static scratch buffer precisely because this is a stack-light path that
 * already lives under nested scripts; capture_program() splits it in place. */
static char subst[LINE_MAX];

static int capture(const char *text, char *out, int cap)
{
    char *av[MAX_ARGS];
    int n = 0;

    expand(text, subst, sizeof(subst), 0);   /* variables, no nested $(...) */
    int ac = split(subst, av, MAX_ARGS);
    if (ac == 0) {
        out[0] = '\0';
        return 0;
    }

    /* The two builtins with an obvious text answer do not need a task.  A
     * program does, and goes through the pipe above. */
    if (!strcmp(av[0], "echo")) {
        for (int i = 1; i < ac; i++) {
            if (i > 1)
                app_str(out, &n, cap, " ");
            app_str(out, &n, cap, av[i]);
        }
        out[n] = '\0';
        last_status = 0;
        return n;
    }
    if (!strcmp(av[0], "pwd")) {
        char buf[64];
        if (getcwd(buf, sizeof(buf)) > 0)
            app_str(out, &n, cap, buf);
        out[n] = '\0';
        last_status = 0;
        return n;
    }
    return capture_program(av, ac, out, cap);
}

/* $name, ${name}, $?, $(cmd).  No quoting and no nested $(...) - this is a
 * command language, not an expression language, and every byte of a second
 * parser comes out of the same window the program's heap lives in. */
static int expand(const char *in, char *out, int cap, int allow_capture)
{
    int n = 0;

    while (*in && n < cap - 1) {
        if (*in != '$') {
            out[n++] = *in++;
            continue;
        }
        in++;
        if (*in == '\0') {
            out[n++] = '$';
            break;
        }
        if (*in == '?') {
            char buf[12];
            str_num(buf, last_status);
            app_str(out, &n, cap, buf);
            in++;
            continue;
        }
        if (*in == '(' && allow_capture) {
            const char *start = ++in;
            while (*in && *in != ')')
                in++;
            if (!*in) {
                sh_err("sh: missing ')' in $(...)\n");
                break;
            }
            int len = (int)(in - start);
            if (len >= LINE_MAX)
                len = LINE_MAX - 1;
            char raw[LINE_MAX], got[LINE_MAX];
            memcpy(raw, start, (size_t)len);
            raw[len] = '\0';
            int room = cap - 1 - n;
            if (room < 0)
                room = 0;
            /* capture() always gets a full-size buffer: it keeps draining
             * the pipe past a short result, and that needs somewhere to
             * write (and a valid out[0]) even when the caller has no room. */
            int g = capture(raw, got, (int)sizeof(got));
            if (g > room)
                g = room;
            memcpy(out + n, got, (size_t)g);
            n += g;
            in++;                   /* past ')' */
            continue;
        }
        /* $name / ${name} */
        char name[VAR_NAME];
        int k = 0, braces = 0;
        if (*in == '{') {
            braces = 1;
            in++;
        }
        while (*in && name_char(*in) && k < VAR_NAME - 1)
            name[k++] = *in++;
        name[k] = '\0';
        if (braces) {
            while (*in && *in != '}')
                in++;
            if (*in == '}')
                in++;
        } else if (k == 0) {
            /* `$ ` or a `$(` in a context that forbids substitution: keep
             * the character as data, and move on - standing still here
             * would spin forever. */
            out[n++] = *in++;
            continue;
        }
        char *v = var_find(name);
        app_str(out, &n, cap, v ? v : "");
    }
    out[n] = '\0';
    if (*in)
        sh_err("sh: line too long after substitution\n");
    return n;
}

/* ---- command dispatch ------------------------------------------------- */

/* Sleep is a builtin inside a script and a program on the console.
 *
 * A script that samples every millisecond cannot afford an exec + ELF
 * relocation per sample.  But a builtin parks the shell itself, and the
 * kernel deliberately refuses to signal init_proc (`process_signal` returns
 * -1 for it, and `process_ctrl_target` skips it), so a `source` script's
 * builtin sleep cannot be interrupted by ^C at all.  Keeping the console
 * path as a real child preserves the foreground-job rule: the shell is
 * ST_WAIT_CHILD, `foreground_job()` finds the sleeper, and ^C takes it.
 * `sleep 500 &`, `nice 2 sleep 500` and `run sleep 500` are programs too.
 * Echo has the same two paths; not blocking, it is harmless either way. */
static int builtin_sleep(const char *arg)
{
    /* Same words as bin/sleep, byte for byte: tests and scripts that look
     * for the rejection message must not care which path handled it. */
    const char *s = arg;
    long v = 0;

    if (!*s) {
        puts("usage: sleep <ms>   (max 3600000)\n");
        last_status = 1;
        return -1;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            break;
        v = v * 10 + (*s - '0');
        if (v > 3600000)
            break;                  /* keep the rest for the message below */
    }
    if (v > 3600000) {
        puts("sleep: longer than 3600000 ms: ");
        puts(arg);
        puts("\n");
        last_status = 1;
        return -1;
    }
    if (*s) {
        puts("sleep: not a number of milliseconds: ");
        puts(arg);
        puts("\n");
        last_status = 1;
        return -1;
    }
    sleep((int)v);
    last_status = 0;
    return -1;
}

/* The console has no script body to jump back to: a control word typed at
 * the prompt is a mistake, not a command to be handed to /bin. */
static int control_word(const char *w)
{
    return !strcmp(w, "if") || !strcmp(w, "while") || !strcmp(w, "else") ||
           !strcmp(w, "fi") || !strcmp(w, "done") || !strcmp(w, "break");
}

/* Run one command line.  Returns -1 to keep going, or the code the shell
 * should end with (`exit`). */
static int run_command(char *s)
{
    char *p = skip_space(s);
    if (!*p || *p == '#')       /* blank line or comment */
        return -1;

    /* `name=value` is checked before '>' and '|': the right hand side has
     * already been substituted and may contain either character as data. */
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
            *gt = '\0';
            return run_redirect(p, gt + 1);
        }
    }

    /* A pipeline is split before tokenising: the '|' separates two command
     * lines, and both halves have to keep their own arguments. */
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

    if (!strcmp(args[0], "help")) {
        cmd_help();
        last_status = 0;
    } else if (!strcmp(args[0], "cd")) {
        /* A builtin, not a program: cwd is per process, so a child that
         * changed directory would take the change with it when it exits. */
        const char *dst = ac > 1 ? args[1] : "/";
        if (chdir(dst) < 0) {
            puts("cd: no such directory: ");
            puts(dst);
            puts("\n");
            last_status = 1;
        } else {
            last_status = 0;
        }
    } else if (!strcmp(args[0], "pwd")) {
        char buf[64];
        if (getcwd(buf, sizeof(buf)) > 0) {
            puts(buf);
            puts("\n");
        }
        last_status = 0;
    } else if (!strcmp(args[0], "rm")) {
        if (ac < 2) {
            puts("usage: rm <file>...\n");
            last_status = 1;
        } else {
            last_status = 0;
            for (int i = 1; i < ac; i++)
                if (unlink(args[i]) < 0) {
                    puts("rm: cannot remove ");
                    puts(args[i]);
                    puts("\n");
                    last_status = 1;
                }
        }
    } else if (!strcmp(args[0], "ps")) {
        /* Through /proc, not through the ps() syscall: the syscall writes
         * straight to the console, so `ps > f` would print on the terminal
         * and leave the file empty. */
        int fd = open("/proc/ps");
        if (fd < 0) {
            ps();               /* no /proc on this build: old path */
        } else {
            char buf[128];
            int n;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                write_all(1, buf, n);
            close(fd);
        }
        last_status = 0;
    } else if (!strcmp(args[0], "exit")) {
        /* On the console `exit` ends the machine; in a script it ends the
         * script, because "quit the whole system" is never what a script
         * written to be run twice means. */
        if (!script_depth) {
            puts("bye.\n");
            exit(0);
        }
        return ac > 1 ? atoi(args[1]) & 255 : 0;
    } else if (!strcmp(args[0], "history")) {
        /* The list lives in the line discipline, not here: it is filled
         * whether or not the shell is the one reading. */
        char h[256];
        int n = hist_count();

        for (int i = n; i >= 1; i--) {
            if (hist_get(i, h, (int)sizeof(h)) < 0)
                continue;
            printf("%4d  %s\n", n - i + 1, h);
        }
        last_status = 0;
        return 0;
    } else if (!strcmp(args[0], "reboot")) {
        puts("rebooting...\n");
        reboot();           /* does not return */
    } else if (!strcmp(args[0], "source")) {
        /* In this process on purpose: the C3 has three task windows in
         * total.  The name is copied because the nested script uses the
         * same command buffer. */
        char name[64];
        if (ac < 2) {
            puts("usage: source <file>\n");
            last_status = 1;
        } else if (strlen(args[1]) >= sizeof(name)) {
            puts("sh: name too long\n");
            last_status = 1;
        } else {
            strcpy(name, args[1]);
            int r = run_script(name);
            if (r < 0)
                cannot_open(name);
            last_status = r < 0 ? 1 : r;
        }
    } else if (!strcmp(args[0], "run")) {
        if (ac < 2)
            puts("usage: run <program> [args...]\n");
        else
            run_program(&args[1], ac - 1, bg);
    } else if (!strcmp(args[0], "nice")) {
        int lvl = ac > 1 ? atoi(args[1]) : -1;
        if (ac < 3 || lvl < 0 || lvl > 2)
            puts("usage: nice <0-2> <program> [args...] [&]\n");
        else
            run_nice(&args[2], ac - 2, bg, lvl);
    } else if (!strcmp(args[0], "prio")) {
        if (ac != 3) {
            puts("usage: prio <pid> <0-2>  (0 highest, 2 lowest)\n");
        } else if (setprio(atoi(args[1]), atoi(args[2])) < 0) {
            puts("prio: no such task, or a level you cannot set\n");
        }
    } else if (control_word(args[0]) && !script_depth) {
        sh_err("sh: if/while/else/fi/done/break need a script\n");
        last_status = 1;
    } else if (!strcmp(args[0], "sleep") && !bg && script_depth > 0) {
        return builtin_sleep(ac > 1 ? args[1] : "");
    } else if (!strcmp(args[0], "echo") && !bg) {
        for (int i = 1; i < ac; i++) {
            if (i > 1)
                putchar(' ');
            puts(args[i]);
        }
        putchar('\n');
        last_status = 0;
    } else if (!strcmp(args[0], "true")) {
        last_status = 0;
    } else if (!strcmp(args[0], "false")) {
        last_status = 1;
    } else if (!strcmp(args[0], "incr")) {
        char buf[12];
        char *v = ac > 1 ? var_find(args[1]) : 0;
        int d = ac > 2 ? atoi(args[2]) : 1;
        if (ac < 2 || !v) {
            sh_err("sh: incr <name> [delta]\n");
            last_status = 1;
        } else {
            str_num(buf, atoi(v) + d);
            var_set(args[1], buf);
            last_status = 0;
        }
    } else {
        /* exec failure details (not found / corrupt / ...) are reported
           by the kernel; do not print a second message here. */
        run_program(args, ac, bg);
    }
    return -1;
}

/* ---- script engine ---------------------------------------------------- */

typedef struct {
    char kind;                  /* 'i' = if, 'w' = while */
    int  start;                 /* line index of the keyword */
    int  elsei;                 /* if: index of else, or -1 */
    int  end;                   /* index of fi / done */
} block_t;

/* Look ahead to the matching terminator.  Blocks are resolved once, when
 * opened, so a false branch and `break` can jump straight past their body
 * without expanding or running a single line of it (important: a condition
 * with $(...) must not run a program inside a branch that is not taken). */
static int scan_block(char *lines[], int n, int start, char kind,
                      int *elsei, int *end)
{
    int depth = 0;
    *elsei = -1;
    *end = -1;
    for (int j = start + 1; j < n; j++) {
        if (word_eq(lines[j], "if") || word_eq(lines[j], "while")) {
            depth++;
        } else if (word_eq(lines[j], "else")) {
            if (depth == 0 && kind == 'i' && *elsei < 0)
                *elsei = j;
        } else if (word_eq(lines[j], "fi") || word_eq(lines[j], "done")) {
            if (depth == 0) {
                if ((kind == 'i' && word_eq(lines[j], "fi")) ||
                    (kind == 'w' && word_eq(lines[j], "done"))) {
                    *end = j;
                    return 0;
                }
                return -1;      /* mismatched terminator */
            }
            depth--;
        }
    }
    return -1;
}

/* One condition, no boolean operators and no quoting.  Text comparison is
 * the default; -eq/-ne/-lt/-gt/-le/-ge use atoi(), so `-gt` on "1.234"
 * compares 1 - fine for the /proc numbers a script is likely to read. */
static int eval_cond(char *s)
{
    char *t[4];
    int n = split(s, t, 4);

    if (n == 0)
        return 0;
    if (n == 1) {
        if (!strcmp(t[0], "-n") || !strcmp(t[0], "-z")) {
            sh_err("sh: -n/-z need an argument\n");
            return 0;
        }
        return t[0][0] != '\0' && strcmp(t[0], "0") != 0;
    }
    if (n == 2 && !strcmp(t[0], "-n"))
        return t[1][0] != '\0';
    if (n == 2 && !strcmp(t[0], "-z"))
        return t[1][0] == '\0';
    if (n == 3) {
        const char *op = t[1];
        int a = atoi(t[0]), b = atoi(t[2]);
        if (!strcmp(op, "==")) return !strcmp(t[0], t[2]);
        if (!strcmp(op, "!=")) return  strcmp(t[0], t[2]) != 0;
        if (!strcmp(op, "-eq")) return a == b;
        if (!strcmp(op, "-ne")) return a != b;
        if (!strcmp(op, "-lt")) return a <  b;
        if (!strcmp(op, "-gt")) return a >  b;
        if (!strcmp(op, "-le")) return a <= b;
        if (!strcmp(op, "-ge")) return a >= b;
    }
    sh_err("sh: bad condition\n");
    return 0;
}

/* Read the whole file into the pool and slice it into lines.  The console
 * cannot do this: it is an endless stream, which is why control statements
 * need a file.  Returns the line count, -1 when the file will not open,
 * -2 for a script that does not fit (already reported). */
static int load_script(const char *path, char *lines[], int maxlines, int *mark)
{
    int fd = open(path);
    if (fd < 0)
        return -1;

    *mark = pool_used;
    int room = POOL_MAX - pool_used - 2;
    if (room <= 0) {
        close(fd);
        sh_err("sh: script text pool full\n");
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
            sh_err("sh: script too long\n");
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
            /* CRLF is one terminator, not an extra empty line. */
            if (p < base + total && (*p == '\n' || *p == '\r'))
                *p++ = '\0';
        }
        char *q = skip_space(s);
        if (!*q || *q == '#')
            continue;           /* blank and comment lines cost no slot */
        if (n >= maxlines) {
            sh_err("sh: too many script lines\n");
            pool_used = *mark;
            return -2;
        }
        lines[n++] = s;
    }
    return n;
}

/* Run a file of command lines in this process: no exec, no new window, no
 * new pid.  On the C3 there are only three task windows in total, and a
 * shell running a script that sources another script that runs a program
 * would need one too many.  `run sh <file>` still exists for the console,
 * where a separate task is what you want (crash isolation, `&`). */
static int run_script(const char *path)
{
    if (script_depth >= MAX_SOURCE) {
        /* Not "cannot open" - the file may be fine, there is just no room
         * left to nest.  Returning 0 keeps that distinction at the caller. */
        puts("sh: scripts nested too deep\n");
        return 0;
    }

    char *lines[MAX_LINES];
    block_t stk[MAX_BLOCKS];
    int mark, n = load_script(path, lines, MAX_LINES, &mark);
    if (n < 0)
        return n;

    script_depth++;
    int code = 0, i = 0, sp = 0, revisit = 0, iters = 0;
    char cmd[LINE_MAX];

    while (i < n) {
        if (strlen(lines[i]) >= LINE_MAX) {
            sh_err("sh: script line too long, truncated\n");
            /* Cut the source too, so expand() does not report the same line
             * a second time as "too long after substitution". */
            lines[i][LINE_MAX - 1] = '\0';
        }
        expand(lines[i], cmd, sizeof(cmd), 1);
        char *head = skip_space(cmd);
        if (!*head || *head == '#') {
            i++;
            continue;
        }

        if (word_eq(head, "if") || word_eq(head, "while")) {
            char kind = word_eq(head, "while") ? 'w' : 'i';
            int elsei, end;
            if (sp >= MAX_BLOCKS ||
                scan_block(lines, n, i, kind, &elsei, &end) < 0) {
                sh_err("sh: unterminated if/while (or nested too deep)\n");
                code = 2;
                break;
            }
            /* `done` jumps back to the `while` line to re-evaluate its
             * condition against variables the body just changed.  Reuse the
             * block instead of pushing it a second time. */
            if (revisit && sp > 0 && stk[sp - 1].kind == 'w' &&
                stk[sp - 1].start == i) {
                revisit = 0;
                block_t *b = &stk[sp - 1];
                int cond = eval_cond(skip_word(head));
                if (++iters > MAX_ITER) {
                    sh_err("sh: loop limit exceeded\n");
                    code = 2;
                    break;
                }
                if (cond) {
                    i = b->start + 1;
                } else {
                    sp--;
                    i = b->end + 1;
                }
                continue;
            }
            block_t b;
            b.kind = kind;
            b.start = i;
            b.elsei = elsei;
            b.end = end;
            stk[sp++] = b;
            int cond = eval_cond(skip_word(head));
            if (cond) {
                i++;
            } else if (kind == 'i' && elsei >= 0) {
                i = elsei + 1;      /* run the else branch, keep the block */
            } else {
                sp--;
                i = end + 1;
            }
            continue;
        }

        if (word_eq(head, "else")) {
            /* Reached only when the true branch ran: skip to after `fi`. */
            if (sp > 0 && stk[sp - 1].kind == 'i') {
                i = stk[sp - 1].end + 1;
                sp--;
            } else {
                sh_err("sh: else without if\n");
                i++;
            }
            continue;
        }
        if (word_eq(head, "fi")) {
            if (sp > 0 && stk[sp - 1].kind == 'i')
                sp--;
            else
                sh_err("sh: fi without if\n");
            i++;
            continue;
        }
        if (word_eq(head, "done")) {
            if (sp > 0 && stk[sp - 1].kind == 'w') {
                revisit = 1;
                i = stk[sp - 1].start;
            } else {
                sh_err("sh: done without while\n");
                i++;
            }
            continue;
        }
        if (word_eq(head, "break")) {
            int j = sp - 1;
            while (j >= 0 && stk[j].kind != 'w')
                j--;
            if (j < 0) {
                sh_err("sh: break outside a loop\n");
                i++;
            } else {
                i = stk[j].end + 1;
                sp = j;             /* drop the loop and everything inside */
            }
            continue;
        }

        int r = run_command(cmd);
        if (r >= 0) {
            code = r;
            break;
        }
        i++;
    }

    script_depth--;
    pool_used = mark;           /* give the bytes back to a nested source */
    return code;
}

int main(int argc, char *argv[])
{
    /* `run sh <file>`: a script, no banner, no prompt. */
    if (argc > 1) {
        int r = run_script(argv[1]);
        if (r < 0) {
            cannot_open(argv[1]);
            return 2;
        }
        return r;
    }

    puts("\nTinyOS shell (pid ");
    putint(getpid());
    puts("). Type 'help' for commands.\n");

    /* Only init runs the boot script.  A sh started from a script would
     * otherwise run it too, and each level costs a whole 64 KiB window. */
    if (getpid() == 1)
        run_script("etc/rc");

    char cmd[LINE_MAX];
    for (;;) {
        print_prompt();
        /* -1 is end of input; on the console it never happens, the read
         * blocks until somebody types something. */
        if (getline(line, sizeof(line)) < 0)
            continue;
        expand(line, cmd, sizeof(cmd), 1);
        run_command(cmd);
    }
}

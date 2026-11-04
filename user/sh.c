#include "lib.h"

#define MAX_ARGS   8
/* `source` nests in this process, so the only limit is the 16 KiB stack. */
#define MAX_SOURCE 4

/* A command line, sized to what the kernel's line discipline will hand over
 * in one piece (TTY_LINE_MAX).  At 128 the shell silently cut anything longer
 * before the kernel ever saw the arguments. */
static char line[256];
/* Note: this is our own storage.  The argv passed to main() only has room
   for the arguments the kernel actually created, so do not reuse it. */
static char *args[MAX_ARGS];

/* Where command lines come from.  A script is the same command loop reading
 * a different source with the prompt turned off - one parser, one dispatcher,
 * two sources.  Anything richer (variables, if/while) would be a second
 * language with a second set of bugs, and every byte of it comes out of the
 * same 64 KiB window the program's heap lives in. */
typedef struct {
    int  fd;                    /* -1: the console */
    int  pos, len;
    /* A file read returns up to len bytes, not one line, so the splitting
     * happens here.  The buffer belongs to the source, not to the shell:
     * `source` nests a second file inside the first and must not stomp on
     * the bytes the outer one has already read but not yet used. */
    char buf[96];
} src_t;

static src_t console = { -1, 0, 0, { 0 } };
/* How many scripts are open.  Two things key off this: `exit` means "end
 * this script" rather than "end the machine", and a runaway `source` chain
 * has to be cut before it eats the 16 KiB stack. */
static int script_depth;

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

/* One line from either source, without its terminator.  -1 is end of input;
 * 0 is an empty line, which is not the same thing - conflating the two ended
 * every script at its first blank line.  The console source never returns -1,
 * it blocks instead. */
static int src_line(src_t *s, char *out, int max)
{
    if (s->fd < 0)
        return getline(out, max);

    int n = 0, trunc = 0;
    for (;;) {
        if (s->pos >= s->len) {
            int r = read(s->fd, s->buf, sizeof(s->buf));
            if (r <= 0) {
                /* EOF.  A partial last line still counts, hence the -1. */
                if (!n)
                    return -1;
                out[n] = '\0';
                return n;
            }
            s->len = r;
            s->pos = 0;
        }
        char c = s->buf[s->pos++];
        if (c == '\n')
            break;
        if (c == '\r')
            continue;
        if (n < max - 1) {
            out[n++] = c;
        } else {
            trunc = 1;              /* keep eating up to the newline */
        }
    }
    out[n] = '\0';
    /* Cutting a script line in silence is how argbuf[64] hid a bug for a
     * week; a script cannot be retyped, so say so. */
    if (trunc)
        puts("sh: script line too long, truncated\n");
    return n;
}

/* Being told nothing at all is the worst answer when a name is wrong. */
static void cannot_open(const char *path)
{
    puts("sh: cannot open ");
    puts(path);
    puts("\n");
}

static void cmd_help(void)
{
    puts("TinyOS shell commands:\n");
    puts("  ls [dir|file]      list a directory, or show a file\n");
    puts("  cd [dir]           change directory (default /)\n");
    puts("  pwd                print the working directory\n");
    puts("  cat <file>         print a file\n");
    puts("  edit <file>        full screen editor (^S save, ^X exit)\n");
    puts("  echo [args...]     print arguments\n");
    puts("  hello              demo program\n");
    puts("  mem                heap / sbrk demo\n");
    puts("  crash              run an illegal instruction (kills that process)\n");
    puts("  uptime             real time since boot (hardware clock)\n");
    puts("  sleep <ms>         wait without using the CPU\n");
    puts("  gpio <pin> [0|1]   read or drive a pin (gpio blink <pin>)\n");
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
    puts("  run <prog> [args]  run a program from /bin\n");
    puts("  run <prog> ... &   run it in the background\n");
    puts("  run sh <file>      run the commands in a file, as a task\n");
    puts("  source <file>      run the commands in a file, in this shell\n");
    puts("  nice <n> <prog> .. run it at priority n (0 high, 2 low)\n");
    puts("  prio <pid> <n>     move a running task to priority n\n");
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

/* exec only spawns a child; the caller decides whether to wait for it. */
static int spawn(char *argv[], int argc)
{
    char path[64];
    path[0] = '\0';
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
    if (pid < 0)
        return pid;     /* the kernel already said why */

    if (bg) {
        puts("[");
        putint(pid);
        puts("]\n");
        return 0;
    }
    return wait(pid);
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
    } else {
        wait(pid);
    }
}

static int run_script(const char *path);

/* A | B: run two programs with a pipe between them.
 *
 *   pipe(f)            two descriptors, read end first
 *   dup2(f[1], 1)      our stdout is the pipe now
 *   exec(A)            the child inherits that table, so its fd 1 is the pipe
 *   close(1)           we are back on the console
 *   dup2(f[0], 0)      our stdin is the read end
 *   exec(B)
 *
 * The shell does the wiring because a child cannot run code between exec()
 * and its first instruction.  It also has to close both of its own copies
 * afterwards: a pipe's EOF means "every write end is gone", and the shell
 * holding one would keep `B` waiting forever.
 *
 * Background pipelines (`A | B &`) are not supported: the `&` would have to
 * decide which of the two it applies to, and the answer is neither. */
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
    if (rpid >= 0)
        wait(rpid);
}

static void sh_err(const char *s)
{
    /* fd 2, not puts(): an error about the redirection must not land in the
     * file the redirection just opened. */
    write(2, s, (int)strlen(s));
}

static int run_command(char *s);

/* `cmd > file` and `cmd > /dev/gpio4`.
 *
 * The shell wires its own fd 1, runs the command, and closes it again - the
 * kernel turns close(1) back into the console.  A spawned child inherits fd 1
 * (see fd_clone in process.c), so the redirection reaches the program without
 * the shell passing it anything, which is the only way it can work: a child
 * cannot run code between exec() and its first instruction.
 *
 * create() first, open() second.  `>` means truncate for a regular file; a
 * device node refuses create() (there is no creating /dev/gpio4 from user
 * space) and opens fine on the retry. */
static int run_redirect(char *cmd, char *target)
{
    int fd, rc;

    while (*target == ' ' || *target == '\t')
        target++;
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

/* Run one command line.  Returns -1 to keep going, or the code the shell
 * should end with (`exit`). */
static int run_command(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '#')       /* blank line or comment */
        return -1;

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
    } else if (!strcmp(args[0], "cd")) {
        /* A builtin, not a program: cwd is per process, so a child that
         * changed directory would take the change with it when it exits. */
        const char *dst = ac > 1 ? args[1] : "/";
        if (chdir(dst) < 0) {
            puts("cd: no such directory: ");
            puts(dst);
            puts("\n");
        }
    } else if (!strcmp(args[0], "pwd")) {
        char buf[64];
        if (getcwd(buf, sizeof(buf)) > 0) {
            puts(buf);
            puts("\n");
        }
    } else if (!strcmp(args[0], "rm")) {
        /* A builtin rather than a /bin program: one syscall per name and a
         * message, which a separate 500-byte binary would not improve.  It
         * also runs before the fork, so `rm` cannot be made to fail for
         * interesting reasons on a full filesystem. */
        if (ac < 2) {
            puts("usage: rm <file>...\n");
        } else {
            for (int i = 1; i < ac; i++)
                if (unlink(args[i]) < 0) {
                    puts("rm: cannot remove ");
                    puts(args[i]);
                    puts("\n");
                }
        }
    } else if (!strcmp(args[0], "ps")) {
        /* Through /proc, not through the ps() syscall: the syscall writes
         * straight to the console, so `ps > f` would print on the terminal
         * and leave the file empty.  Reading the file means the redirect
         * works and there is one implementation of the table, not two. */
        int fd = open("/proc/ps");
        if (fd < 0) {
            ps();               /* no /proc on this build: old path */
        } else {
            char buf[128];
            int n;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                write(1, buf, n);
            close(fd);
        }
    } else if (!strcmp(args[0], "exit")) {
        /* On the console `exit` ends the machine; in a script it ends the
         * script, because "quit the whole system" is never what a script
         * written to be run twice means. */
        if (!script_depth) {
            puts("bye.\n");
            exit(0);
        }
        return ac > 1 ? atoi(args[1]) & 255 : 0;
    } else if (!strcmp(args[0], "reboot")) {
        puts("rebooting...\n");
        reboot();           /* does not return */
    } else if (!strcmp(args[0], "source")) {
        /* In this process on purpose: see run_script().  The name is copied
         * out of `line` first because the nested script reuses that buffer. */
        char name[64];
        if (ac < 2) {
            puts("usage: source <file>\n");
        } else if (strlen(args[1]) >= sizeof(name)) {
            puts("sh: name too long\n");
        } else {
            strcpy(name, args[1]);
            if (run_script(name) < 0)
                cannot_open(name);
        }
    } else if (!strcmp(args[0], "run")) {
        if (ac < 2) {
            puts("usage: run <program> [args...]\n");
        } else {
            run_program(&args[1], ac - 1, bg);
        }
    } else if (!strcmp(args[0], "nice")) {
        /* nice <level> <program> [args...] [&] */
        int lvl = ac > 1 ? atoi(args[1]) : -1;
        if (ac < 3 || lvl < 0 || lvl > 2)
            puts("usage: nice <0-2> <program> [args...] [&]\n");
        else
            run_nice(&args[2], ac - 2, bg, lvl);
    } else if (!strcmp(args[0], "prio")) {
        /* prio <pid> <level>: move a task that is already running */
        if (ac != 3) {
            puts("usage: prio <pid> <0-2>  (0 highest, 2 lowest)\n");
        } else if (setprio(atoi(args[1]), atoi(args[2])) < 0) {
            puts("prio: no such task, or a level you cannot set\n");
        }
    } else {
        /* exec failure details (not found / corrupt / ...) are reported
           by the kernel; do not print a second message here. */
        run_program(args, ac, bg);
    }
    return -1;
}

/* Read and run a file of command lines, in this process: no exec, no new
 * window, no new pid.  That is not a shortcut, it is the only thing that
 * works on the C3: it has three task windows in total (shell, foreground,
 * spare), so a shell running a script that sources another script that runs
 * any program at all is one window too many - measured, the answer is
 * "exec: out of task windows".  `run sh <file>` still exists for the
 * console, where a separate task is what you want (crash isolation, `&`).
 *
 * `line` is shared with the outer level.  That is safe because a command
 * line is fully parsed before anything can nest, and the caller has no use
 * for it afterwards; `source` copies its argument out for the same reason. */
static int run_script(const char *path)
{
    if (script_depth >= MAX_SOURCE) {
        /* Not an "cannot open" - the file may be fine, there is just no room
         * left to nest.  Returning 0 keeps that distinction at the callers. */
        puts("sh: scripts nested too deep\n");
        return 0;
    }

    src_t s;
    s.fd = open(path);
    if (s.fd < 0)
        return -1;
    s.pos = s.len = 0;
    script_depth++;

    int code = 0;
    for (;;) {
        int n = src_line(&s, line, sizeof(line));
        if (n < 0)
            break;
        int r = run_command(line);
        if (r >= 0) {
            code = r;
            break;
        }
    }

    script_depth--;
    close(s.fd);
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

    for (;;) {
        print_prompt();
        /* -1 is end of input; on the console it never happens, the read
         * blocks until somebody types something. */
        if (src_line(&console, line, sizeof(line)) < 0)
            continue;
        run_command(line);
    }
}

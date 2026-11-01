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
    puts("  ls                 list files in the ramdisk\n");
    puts("  cat <file>         print a file\n");
    puts("  echo [args...]     print arguments\n");
    puts("  hello              demo program\n");
    puts("  mem                heap / sbrk demo\n");
    puts("  crash              run an illegal instruction (kills that process)\n");
    puts("  uptime             time since boot\n");
    puts("  ps                 list tasks\n");
    puts("  run <prog> [args]  run a program from /bin\n");
    puts("  run <prog> ... &   run it in the background\n");
    puts("  run sh <file>      run the commands in a file, as a task\n");
    puts("  source <file>      run the commands in a file, in this shell\n");
    puts("  help               this text\n");
    puts("  reboot             reset the machine and boot again\n");
    puts("  exit [code]        shut TinyOS down (end the script, in a script)\n");
}

/* exec only spawns a child; the caller decides whether to wait for it. */
static int run_program(char *argv[], int argc, int bg)
{
    char path[64];
    path[0] = '\0';
    strcpy(path, "bin/");
    if (strlen(path) + strlen(argv[0]) >= sizeof(path)) {
        puts("sh: name too long\n");
        return -1;
    }
    strcpy(path + 4, argv[0]);

    int pid = exec(path, argv, argc);
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

static int run_script(const char *path);

/* Run one command line.  Returns -1 to keep going, or the code the shell
 * should end with (`exit`). */
static int run_command(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '#')       /* blank line or comment */
        return -1;

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
    } else if (!strcmp(args[0], "ps")) {
        ps();
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
        puts("tos> ");
        /* -1 is end of input; on the console it never happens, the read
         * blocks until somebody types something. */
        if (src_line(&console, line, sizeof(line)) < 0)
            continue;
        run_command(line);
    }
}

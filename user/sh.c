#include "lib.h"

#define MAX_ARGS 8

static char line[128];
/* Note: this is our own storage.  The argv passed to main() only has room
   for the arguments the kernel actually created, so do not reuse it. */
static char *args[MAX_ARGS];

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
    puts("  help               this text\n");
    puts("  exit               shut TinyOS down\n");
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

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    puts("\nTinyOS shell (pid ");
    putint(getpid());
    puts("). Type 'help' for commands.\n");

    for (;;) {
        puts("tos> ");
        int n = getline(line, sizeof(line));
        if (n <= 0)
            continue;

        int ac = split(line, args, MAX_ARGS);
        if (ac == 0)
            continue;

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
            puts("bye.\n");
            exit(0);
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
    }
}

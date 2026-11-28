#include "lib.h"

/* top - refresh the task table until ^C.
 *
 *   top [-d ms] [-n count]
 *
 * The kernel tty passes CSI sequences through without interpreting them
 * (edit draws its screen the same way), so on a console top clears and
 * homes before each refresh and erases the rest of the screen after the
 * table.  When stdout is a file the escapes would be garbage, so fstat
 * decides: only a character device gets them, `top -n 1 > f` stays a
 * plain snapshot.  -n stops after that many refreshes; without it top
 * runs until ^C, which the kernel delivers as SIGKILL to the foreground
 * task.
 *
 * The table is /proc/ps, the same bytes `ps` writes, so this stays correct
 * when stdout is redirected.  It is a separate program rather than an applet
 * in /bin/tools: the multi-call binary sits 192 bytes under its 12 KiB span
 * boundary on rv32, and a top that loops for minutes does not want to hold
 * that window anyway.
 */
TOS_STACK(4 * 1024);

/* Whether stdout is the console rather than a file: the escapes are for a
 * terminal, and a redirected snapshot must stay plain text. */
static int on_console(void)
{
    struct stat st;

    return fstat(1, &st) == 0 && st.type == TOS_FTYPE_CHR;
}

static void dump_ps(void)
{
    char buf[128];
    int fd = open("/proc/ps");
    int n;

    if (fd < 0) {
        ps();                       /* no /proc on this build: old path */
        return;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write_all(1, buf, n) != n)
            break;
    }
    close(fd);
}

int main(int argc, char **argv)
{
    int interval = 1000, count = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc)
            interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            count = atoi(argv[++i]);
        else {
            puts("usage: top [-d ms] [-n count]\n");
            return 2;
        }
    }
    if (interval < 10)
        interval = 10;              /* below a tick the CPU never idles */
    if (interval > 3600000)
        interval = 3600000;

    int tty = on_console();

    for (int round = 0; count <= 0 || round < count; round++) {
        int ms = uptime();

        if (tty)
            puts("\x1b[2J\x1b[H");
        else
            putchar('\n');
        printf("# top %d.%03d s  pid %d  refresh %d ms  ^C to stop\n",
               ms / 1000, ms % 1000, getpid(), interval);
        dump_ps();
        if (tty)
            puts("\x1b[J");
        if (count > 0 && round + 1 >= count)
            break;
        sleep(interval);
    }
    return 0;
}

/* kill - send a signal to a task by pid.
 *
 *   kill <pid>...           SIGKILL, the same signal the console's ^C sends
 *   kill -9 <pid>...        SIGKILL
 *   kill -STOP <pid>...     park it until SIGCONT
 *   kill -CONT <pid>...     resume a parked task
 *
 * The kernel already has SYS_SIGNAL (what ^C uses); this is only its command
 * line.  Exit status is 0 when every pid was found, 1 when one was not, 2 on
 * a usage error.  `ps` prints the pids.
 */
#include "lib.h"

static int signal_named(const char *s, int *sig)
{
    if (!strcmp(s, "-9") || !strcmp(s, "-KILL") || !strcmp(s, "-kill")) {
        *sig = SIGKILL;
        return 0;
    }
    if (!strcmp(s, "-STOP") || !strcmp(s, "-stop")) {
        *sig = SIGSTOP;
        return 0;
    }
    if (!strcmp(s, "-CONT") || !strcmp(s, "-cont")) {
        *sig = SIGCONT;
        return 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    int sig = SIGKILL;
    int i = 1;
    int rc = 0;

    if (i < argc && argv[i][0] == '-') {
        if (signal_named(argv[i], &sig) < 0) {
            printf("kill: unknown signal %s\n", argv[i]);
            return 2;
        }
        i++;
    }
    if (i >= argc) {
        puts("usage: kill [-9|-STOP|-CONT] pid...\n");
        return 2;
    }
    for (; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (pid <= 0 || kill(pid, sig) < 0) {
            printf("kill: %s: no such task\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

/* mailtest - a parent and a worker exchanging small structs through the
 * global mailbox pool.  Enable it with mailbox = 1 (menuconfig: Kernel ->
 * Mailbox); the worker is the same binary in -w mode. */

#include "lib.h"

#ifdef TOS_MAILBOX

TOS_STACK(4 * 1024);

struct msg {
    int from;
    int value;
    int cmd;        /* 1 = double this, 0 = stop */
};

static void dec(char *buf, int v)
{
    char tmp[12];
    int i = 0;

    do {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0 && i < (int)sizeof(tmp));
    for (int k = 0; k < i; k++)
        buf[k] = tmp[i - 1 - k];
    buf[i] = '\0';
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "-w")) {
        int ppid = atoi(argv[2]);

        for (;;) {
            struct msg r;
            int n = msg_recv(&r, (int)sizeof(r));

            if (n != (int)sizeof(r))
                return 1;
            if (r.cmd == 0)
                return 0;
            r.value *= 2;
            r.cmd = 1;
            r.from = getpid();
            while (msg_send(ppid, &r, (int)sizeof(r)) < 0)
                sleep(1);           /* pool full: retry */
        }
    }

    char pidbuf[12];
    char *args[3];
    dec(pidbuf, getpid());
    args[0] = "mailtest";
    args[1] = "-w";
    args[2] = pidbuf;

    int pid = exec("bin/mailtest", args, 3);
    if (pid < 0) {
        puts("mailtest: cannot spawn worker\n");
        return 1;
    }

    for (int i = 1; i <= 3; i++) {
        struct msg r;

        r.from = getpid();
        r.value = i;
        r.cmd = 1;
        while (msg_send(pid, &r, (int)sizeof(r)) < 0)
            sleep(1);
    }

    for (int i = 0; i < 3; i++) {
        struct msg r;

        if (msg_recv(&r, (int)sizeof(r)) != (int)sizeof(r)) {
            puts("mailtest: bad reply\n");
            return 1;
        }
        printf("reply %d -> %d\n", r.value / 2, r.value);
    }

    struct msg stop;
    stop.from = getpid();
    stop.value = 0;
    stop.cmd = 0;
    msg_send(pid, &stop, (int)sizeof(stop));
    wait(pid);
    return 0;
}

#else

int main(void)
{
    puts("mailbox is off (menuconfig: Kernel -> Mailbox)\n");
    return 0;
}

#endif

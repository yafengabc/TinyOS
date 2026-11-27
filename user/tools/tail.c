/* tail - print the last N lines of a file.
 *
 *   tail [-n N] <file>
 *
 * Files only: finding the tail of a stream would need a seek or a buffer
 * that grows with the file.  The scan remembers the last N+1 line starts
 * (offsets), then lseeks to the oldest of them and copies to the end, so the
 * memory cost is 400 bytes regardless of file size.
 */
#include "lib.h"

#define TAIL_MAX 100

int applet_tail(int argc, char **argv)
{
    int lines = 10;
    int i = 1;
    int starts[TAIL_MAX + 1];
    int count = 0, next = 0, off = 0, n;
    char buf[256];
    int fd, start;

    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        lines = atoi(argv[i + 1]);
        i += 2;
    }
    if (lines < 1)
        lines = 1;
    if (lines > TAIL_MAX)
        lines = TAIL_MAX;
    if (i >= argc) {
        puts("usage: tail [-n N] <file>\n");
        return 2;
    }

    fd = open(argv[i]);
    if (fd < 0) {
        printf("tail: %s: no such file\n", argv[i]);
        return 1;
    }

    /* The first line starts at 0; every newline starts the next one.  The
     * ring holds the last lines+1 starts, so when it is full the next write
     * position is the oldest entry. */
    starts[next++] = 0;
    count = 1;
    next %= lines + 1;

    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (int k = 0; k < n; k++) {
            off++;
            if (buf[k] == '\n') {
                starts[next] = off;
                next = (next + 1) % (lines + 1);
                if (count < lines + 1)
                    count++;
            }
        }
    }
    if (n < 0) {
        printf("tail: %s: read error\n", argv[i]);
        close(fd);
        return 1;
    }

    start = (count == lines + 1) ? starts[next] : 0;
    if (lseek(fd, start, SEEK_SET) < 0) {
        printf("tail: %s: seek error\n", argv[i]);
        close(fd);
        return 1;
    }
    while ((n = read(fd, buf, sizeof buf)) > 0)
        if (write_all(1, buf, n) != n) {
            close(fd);
            return 1;
        }
    close(fd);
    return n < 0;
}

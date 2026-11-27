/* head - print the first N lines.
 *
 *   head [-n N] [file...]
 *
 * With no file it reads standard input, so it works at the head of a pipe.
 * A line longer than the output buffer is still copied whole; only the line
 * count stops it.
 */
#include "lib.h"

static int head_fd(int fd, int lines)
{
    char in[256];
    char out[256];
    int olen = 0, n, left = lines;

    while (left > 0 && (n = read(fd, in, sizeof in)) > 0) {
        for (int i = 0; i < n && left > 0; i++) {
            char c = in[i];

            if (olen == (int)sizeof out) {
                if (write_all(1, out, olen) != olen)
                    return -1;
                olen = 0;
            }
            out[olen++] = c;
            if (c == '\n')
                left--;
        }
    }
    if (olen > 0 && write_all(1, out, olen) != olen)
        return -1;
    return n < 0 ? -1 : 0;
}

int applet_head(int argc, char **argv)
{
    int lines = 10;
    int i = 1;
    int rc = 0;

    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        lines = atoi(argv[i + 1]);
        i += 2;
    }
    if (lines < 1)
        lines = 1;

    if (i >= argc)
        return head_fd(0, lines) < 0;

    for (; i < argc; i++) {
        int fd = open(argv[i]);

        if (fd < 0) {
            printf("head: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (head_fd(fd, lines) < 0) {
            printf("head: %s: read error\n", argv[i]);
            rc = 1;
        }
        close(fd);
    }
    return rc;
}

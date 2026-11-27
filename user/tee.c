/* tee - copy standard input to standard output and to files.
 *
 *   tee [-a] [file...]
 *
 * create() truncates, so -a opens first and seeks to the end.  Up to eight
 * files are accepted; the console's write is what a pipeline reads, so it is
 * written first and a short write stops the copy.
 */
#include "lib.h"

#define TEE_MAX 8

int main(int argc, char **argv)
{
    int append = 0;
    int i = 1, nf = 0;
    int fds[TEE_MAX];
    char buf[256];
    int n;

    if (i < argc && !strcmp(argv[i], "-a")) {
        append = 1;
        i++;
    }
    for (; i < argc; i++) {
        int fd;

        if (nf == TEE_MAX) {
            puts("tee: too many files\n");
            return 1;
        }
        if (append) {
            fd = open(argv[i]);
            if (fd >= 0 && lseek(fd, 0, SEEK_END) < 0) {
                printf("tee: %s: seek error\n", argv[i]);
                close(fd);
                return 1;
            }
        } else {
            fd = -1;
        }
        if (fd < 0)
            fd = create(argv[i]);
        if (fd < 0) {
            printf("tee: %s: cannot create\n", argv[i]);
            return 1;
        }
        fds[nf++] = fd;
    }

    while ((n = read(0, buf, sizeof buf)) > 0) {
        if (write_all(1, buf, n) != n) {
            n = -1;
            break;
        }
        for (int k = 0; k < nf; k++)
            if (write_all(fds[k], buf, n) != n) {
                printf("tee: write error on file %d\n", k);
                n = -1;
                break;
            }
        if (n < 0)
            break;
    }
    for (int k = 0; k < nf; k++)
        close(fds[k]);
    return n < 0;
}

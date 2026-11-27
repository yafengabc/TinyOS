/* cp - copy one file.
 *
 *   cp <src> <dst>
 *
 * create() truncates dst, so copying a file onto itself would destroy it;
 * that case is refused rather than "succeeding" with an empty file.
 */
#include "lib.h"

int main(int argc, char **argv)
{
    char buf[256];
    int in, out, n, rc = 0;

    if (argc != 3) {
        puts("usage: cp <src> <dst>\n");
        return 2;
    }
    if (!strcmp(argv[1], argv[2])) {
        printf("cp: %s and %s are the same file\n", argv[1], argv[2]);
        return 1;
    }
    in = open(argv[1]);
    if (in < 0) {
        printf("cp: %s: no such file\n", argv[1]);
        return 1;
    }
    out = create(argv[2]);
    if (out < 0) {
        printf("cp: %s: cannot create\n", argv[2]);
        close(in);
        return 1;
    }
    while ((n = read(in, buf, sizeof buf)) > 0) {
        if (write_all(out, buf, n) != n) {
            printf("cp: %s: write error\n", argv[2]);
            rc = 1;
            break;
        }
    }
    if (n < 0) {
        printf("cp: %s: read error\n", argv[1]);
        rc = 1;
    }
    close(in);
    close(out);
    return rc;
}

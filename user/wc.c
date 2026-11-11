#include "lib.h"

/* wc - count lines, words and bytes.
 *
 *   wc                 count standard input (the pipe case)
 *   wc <file>...       count each named file
 *
 * With no arguments it reads fd 0 until EOF, which is what makes it useful
 * at the end of a pipeline: `cat readme.txt | wc` exercises the pipe in both
 * directions.  With arguments it opens each file itself, so `wc readme.txt`
 * reports on the file instead of waiting for a stdin that will never close.
 */

struct counts {
    unsigned int lines;
    unsigned int words;
    unsigned int bytes;
};

static int count_fd(int fd, struct counts *c)
{
    char buf[128];
    int n;
    int in_word = 0;

    c->lines = c->words = c->bytes = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char ch = buf[i];

            c->bytes++;
            if (ch == '\n')
                c->lines++;
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                c->words++;
            }
        }
    }
    return n < 0 ? -1 : 0;
}

static void report(const char *name, const struct counts *c)
{
    if (name)
        printf("%d %d %d %s\n", (int)c->lines, (int)c->words,
               (int)c->bytes, name);
    else
        printf("%d %d %d\n", (int)c->lines, (int)c->words, (int)c->bytes);
}

int main(int argc, char *argv[])
{
    struct counts c, total;

    if (argc < 2) {
        if (count_fd(0, &c) < 0) {
            puts("wc: read error\n");
            return 1;
        }
        report(0, &c);
        return 0;
    }

    total.lines = total.words = total.bytes = 0;
    int rc = 0;

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i]);

        if (fd < 0) {
            printf("wc: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (count_fd(fd, &c) < 0) {
            printf("wc: %s: read error\n", argv[i]);
            rc = 1;
        } else {
            report(argv[i], &c);
            total.lines += c.lines;
            total.words += c.words;
            total.bytes += c.bytes;
        }
        close(fd);
    }

    if (argc > 2)
        report("total", &total);
    return rc;
}

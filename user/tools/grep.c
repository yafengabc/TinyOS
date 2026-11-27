/* grep - print the lines that contain a substring.
 *
 *   grep [-i] [-n] [-v] pattern [file...]
 *
 * Plain substring matching, not a regexp engine: the common case on a board
 * is "which file mentions this symbol".  -i folds case, -n numbers the
 * lines, -v inverts.  With no file it reads standard input, and with more
 * than one file it prefixes each hit with the name.  A line longer than 511
 * bytes is matched on its first 511 and the rest is dropped.
 */
#include "lib.h"

static char fold(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int contains_ci(const char *hay, const char *needle)
{
    int nl = (int)strlen(needle);

    for (int i = 0; hay[i]; i++) {
        int k = 0;

        while (k < nl && hay[i + k] && fold(hay[i + k]) == fold(needle[k]))
            k++;
        if (k == nl)
            return 1;
    }
    return 0;
}

static void report(const char *line, int lineno, const char *pat, int ci,
                   int num, int invert, const char *name, int many)
{
    int hit = ci ? contains_ci(line, pat) : strstr(line, pat) != 0;

    if (invert)
        hit = !hit;
    if (!hit)
        return;
    if (many)
        printf("%s:", name);
    if (num)
        printf("%d:", lineno);
    printf("%s\n", line);
}

static int grep_fd(int fd, const char *pat, int ci, int num, int invert,
                   const char *name, int many)
{
    char in[256];
    char line[512];
    int len = 0, lineno = 0, n;

    while ((n = read(fd, in, sizeof in)) > 0) {
        for (int i = 0; i < n; i++) {
            char c = in[i];

            if (c == '\n') {
                line[len] = '\0';
                report(line, ++lineno, pat, ci, num, invert, name, many);
                len = 0;
            } else if (len < (int)sizeof line - 1) {
                line[len++] = c;
            }
        }
    }
    if (len > 0) {
        line[len] = '\0';
        report(line, ++lineno, pat, ci, num, invert, name, many);
    }
    return n < 0 ? 1 : 0;
}

int applet_grep(int argc, char **argv)
{
    int ci = 0, num = 0, invert = 0;
    int i = 1, rc = 0;
    const char *pat;
    int files;

    for (; i < argc && argv[i][0] == '-'; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'i':
                ci = 1;
                break;
            case 'n':
                num = 1;
                break;
            case 'v':
                invert = 1;
                break;
            default:
                printf("grep: unknown option -%c\n", *o);
                return 2;
            }
        }
    }
    if (i >= argc) {
        puts("usage: grep [-i] [-n] [-v] pattern [file...]\n");
        return 2;
    }
    pat = argv[i++];
    files = argc - i;
    if (files == 0)
        return grep_fd(0, pat, ci, num, invert, "(stdin)", 0);

    for (; i < argc; i++) {
        int fd = open(argv[i]);

        if (fd < 0) {
            printf("grep: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (grep_fd(fd, pat, ci, num, invert, argv[i], files > 1))
            rc = 1;
        close(fd);
    }
    return rc;
}

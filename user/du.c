/* du - report how many bytes a file tree occupies.
 *
 *   du [path...]
 *
 * Directories are walked with opendir/readdir and the sizes of the regular
 * files are summed.  The filesystem allows four directory streams at once,
 * so the walk refuses to go deeper than three levels rather than failing
 * halfway with a stream exhaustion.
 */
#include "lib.h"

#define DU_MAX_DEPTH 3

static unsigned long du_path(const char *path, int depth)
{
    struct stat st;
    unsigned long total = 0;

    if (stat(path, &st) < 0)
        return 0;
    if (st.type != TOS_FTYPE_DIR)
        return st.size;
    if (depth > DU_MAX_DEPTH) {
        printf("du: %s: too deep\n", path);
        return 0;
    }

    DIR *d = opendir(path);
    if (!d) {
        printf("du: %s: cannot open\n", path);
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char child[256];
        const char *sep = (path[0] && path[strlen(path) - 1] == '/') ? "" : "/";

        if (snprintf(child, sizeof child, "%s%s%s", path, sep, e->d_name)
                >= (int)sizeof child) {
            printf("du: %s: path too long\n", e->d_name);
            continue;
        }
        total += du_path(child, depth + 1);
    }
    closedir(d);
    return total;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        unsigned long total = du_path(".", 0);

        printf("%u .\n", (unsigned)total);
        return 0;
    }
    for (int i = 1; i < argc; i++)
        printf("%u %s\n", (unsigned)du_path(argv[i], 0), argv[i]);
    return 0;
}

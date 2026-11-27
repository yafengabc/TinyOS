/* mkdir - create directories.
 *
 *   mkdir <dir>...
 *   mkdir -p <dir>...      create missing parents too
 *
 * Without -p the parent has to exist, exactly like the POSIX tool.  With -p
 * the intermediate mkdir calls are allowed to fail (they usually "fail"
 * because the directory is already there); only the final component has to
 * succeed.  The path cap is the kernel's TOS_PATH_MAX.
 */
#include "lib.h"

static int make_parents(char *path)
{
    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(path);            /* EEXIST is the normal answer */
            *p = '/';
        }
    return 0;
}

int applet_mkdir(int argc, char **argv)
{
    int parents = 0;
    int i = 1;
    int rc = 0;

    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-p")) {
            parents = 1;
        } else {
            printf("mkdir: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (i >= argc) {
        puts("usage: mkdir [-p] <dir>...\n");
        return 2;
    }
    for (; i < argc; i++) {
        char path[TOS_PATH_MAX];

        if (strlen(argv[i]) >= sizeof path) {
            printf("mkdir: %s: path too long\n", argv[i]);
            rc = 1;
            continue;
        }
        strcpy(path, argv[i]);
        if (parents)
            make_parents(path);
        if (mkdir(path) < 0) {
            printf("mkdir: %s: cannot create\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

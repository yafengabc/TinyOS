/* rmdir - remove empty directories.
 *
 *   rmdir <dir>...
 *
 * The kernel refuses a non-empty directory and any directory that is (or
 * contains) the caller's cwd; this only reports which argument failed.
 */
#include "lib.h"

int applet_rmdir(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {
        puts("usage: rmdir <dir>...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++)
        if (rmdir(argv[i]) < 0) {
            printf("rmdir: %s: cannot remove (not empty, or in use)\n", argv[i]);
            rc = 1;
        }
    return rc;
}

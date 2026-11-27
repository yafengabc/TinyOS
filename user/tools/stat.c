/* stat - report a file's type and size.
 *
 *   stat <path>...
 *
 * The filesystem has no inode numbers or timestamps (struct stat is just
 * size + type), so that is all there is to print.
 */
#include "lib.h"

int applet_stat(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {
        puts("usage: stat <path>...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        struct stat st;

        if (stat(argv[i], &st) < 0) {
            printf("stat: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (st.type == TOS_FTYPE_DIR)
            printf("%s: directory\n", argv[i]);
        else if (st.type == TOS_FTYPE_CHR)
            printf("%s: device\n", argv[i]);
        else
            printf("%s: file, %u bytes\n", argv[i], st.size);
    }
    return rc;
}

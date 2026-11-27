/* stat - report a file's type and size.
 *
 *   stat <path>...
 *
 * The filesystem has no inode numbers or timestamps (struct stat is just
 * size + type), so that is all there is to print.
 */
#include "lib.h"

int main(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {
        puts("usage: stat <path>...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        struct stat st;
        const char *kind;

        if (stat(argv[i], &st) < 0) {
            printf("stat: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if (st.type == TOS_FTYPE_DIR)
            kind = "directory";
        else if (st.type == TOS_FTYPE_CHR)
            kind = "device";
        else
            kind = "file";
        printf("%s: %s, %u bytes\n", argv[i], kind, st.size);
    }
    return rc;
}

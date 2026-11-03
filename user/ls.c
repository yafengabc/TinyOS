#include "lib.h"

/* One entry, formatted like the original: a type letter, the name (with a
 * trailing slash for directories), padded out to a fixed column, then the
 * size, or "<dir>" / "<dev>" for things that have no byte count.  printf has
 * no %Ns, so the padding is done by hand. */
static void print_entry(const char *name, struct stat *st)
{
    int isdir = (st->type == TOS_FTYPE_DIR);
    int ischr = (st->type == TOS_FTYPE_CHR);
    char kind = isdir ? 'd' : (ischr ? 'c' : '-');
    int disp  = (int)strlen(name) + (isdir ? 1 : 0);

    printf("%c  %s%s", kind, name, isdir ? "/" : "");
    for (int p = disp; p < 24; p++)
        putchar(' ');

    if (isdir)
        printf("  <dir>\n");
    else if (ischr)
        printf("  <dev>\n");
    else
        printf("  %u bytes\n", st->size);
}

/* `ls` with no argument lists the current working directory.  An argument is
 * either a single entry (printed on its own) or a directory (moved into with
 * chdir, after which readdir lists the cwd).  Because readdir lists the cwd,
 * `ls <dir>` needs no new syscall: chdir there, list, and the child's cwd
 * disappears when ls exits without disturbing the shell. */
int main(int argc, char **argv)
{
    const char *target = (argc > 1) ? argv[1] : ".";

    struct stat st;
    if (stat(target, &st) == 0 && st.type != TOS_FTYPE_DIR) {
        print_entry(target, &st);
        return 0;
    }

    if (chdir(target) < 0) {
        puts("ls: cannot access ");
        puts(target);
        puts("\n");
        return 1;
    }

    char name[64];
    struct stat es;
    for (int i = 0;; i++) {
        int n = readdir(i, name, sizeof(name));
        if (n <= 0)
            break;
        if (stat(name, &es) != 0)
            continue;
        print_entry(name, &es);
    }
    return 0;
}

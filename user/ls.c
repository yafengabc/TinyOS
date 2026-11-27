#include "lib.h"
#include "tools/applets.h"

/* One entry in long form: a type letter, the name (with a trailing slash for
 * directories), padded out to a fixed column, then the size, or "<dir>" /
 * "<dev>" for things that have no byte count.  printf has no %Ns, so the
 * padding is done by hand. */
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

/* Default form: names run together, two spaces apart, wrapped at 78 columns.
 * `column` is the current line length; a program runs once, so a static is
 * enough. */
static int column;

static void print_name(const char *name, int isdir)
{
    int len = (int)strlen(name) + (isdir ? 1 : 0);

    if (column != 0 && column + 2 + len > 78) {
        putchar('\n');
        column = 0;
    }
    if (column != 0) {
        putchar(' ');
        putchar(' ');
        column += 2;
    }
    printf("%s%s", name, isdir ? "/" : "");
    column += len;
}

/* `ls` lists the current directory; `ls <dir>` chdirs into it first (the
 * child's cwd disappears when ls exits, so the shell is unaffected).
 * `-l` selects the long form, one entry per line. */
int main(int argc, char **argv)
{
    const char *target = ".";
    int long_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) {
            long_mode = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            puts("ls: unknown option ");
            puts(argv[i]);
            puts("\n");
            return 1;
        } else {
            target = argv[i];
        }
    }

    struct stat st;
    if (stat(target, &st) == 0 && st.type != TOS_FTYPE_DIR) {
        if (long_mode)
            print_entry(target, &st);
        else {
            print_name(target, 0);
            putchar('\n');
        }
        return 0;
    }

    if (chdir(target) < 0) {
        puts("ls: cannot access ");
        puts(target);
        puts("\n");
        return 1;
    }

    char name[TOS_PATH_MAX];
    struct stat es;
    for (int i = 0;; i++) {
        int n = readdir_index(i, name, sizeof(name));
        if (n <= 0)
            break;
        if (stat(name, &es) != 0)
            continue;
        if (long_mode)
            print_entry(name, &es);
        else
            print_name(name, es.type == TOS_FTYPE_DIR);
    }

    /* The small commands are applets in /bin/tools; the long listing shows
     * them as aliases so `ls -l /bin` still answers "where is du".  The
     * default listing stays the raw directory: scripts count those names, and
     * `cp -> tools` would change every count. */
    if (long_mode && stat("tools", &es) == 0 && es.type == TOS_FTYPE_REG) {
        static const char *const applets[] = {
#define APPLET_NAME(n) #n,
            TOS_APPLETS(APPLET_NAME)
#undef APPLET_NAME
            NULL
        };
        for (int i = 0; applets[i]; i++)
            printf("l  %s -> tools\n", applets[i]);
    }

    if (!long_mode && column != 0)
        putchar('\n');
    return 0;
}

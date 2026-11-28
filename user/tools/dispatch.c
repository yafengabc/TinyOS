/* tools - one binary behind the small commands.
 *
 * The shell maps cp/head/tail/grep/tee/stat/du/kill/mkdir/rmdir to
 * /bin/tools and passes the command name as argv[0]; `tools <name> ...`
 * works as well.  One file instead of ten: every littlefs file costs at
 * least one 4 KiB block, and each separate program also links its own copy
 * of the C library.  `tools` with no argument lists the applets.
 */
#include "lib.h"
#include "tools/applets.h"

/* One note for the whole multi-call binary: no applet allocates and the
 * big buffers are static, so 4 KiB covers the deepest chain.  No grow
 * reserve - the window is exact. */
TOS_STACK(4 * 1024);

#define DECL(n) int applet_##n(int argc, char **argv);
TOS_APPLETS(DECL)
#undef DECL

struct applet {
    const char *name;
    int (*fn)(int, char **);
};

static const struct applet applets[] = {
#define ENTRY(n) { #n, applet_##n },
    TOS_APPLETS(ENTRY)
#undef ENTRY
};

#define NAPPLETS ((int)(sizeof applets / sizeof applets[0]))

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');

    return s ? s + 1 : p;
}

int main(int argc, char **argv)
{
    const char *name;

    if (argc < 1)
        return 2;
    name = base_name(argv[0]);
    if (!strcmp(name, "tools")) {
        if (argc < 2) {
            puts("applets:");
            for (int i = 0; i < NAPPLETS; i++) {
                putchar(' ');
                puts(applets[i].name);
            }
            puts("\n");
            return 2;
        }
        argv++;
        argc--;
        name = argv[0];
    }
    for (int i = 0; i < NAPPLETS; i++)
        if (!strcmp(applets[i].name, name))
            return applets[i].fn(argc, argv);
    printf("tools: unknown applet %s\n", name);
    return 2;
}

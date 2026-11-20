#include "lib.h"
#include "avm.h"

#define AVM_BLOCK (8 * 1024)

TOS_STACK(4 * 1024);

static uint8_t block[AVM_BLOCK];
static avm vm;

static acell n_print(avm *vm, acell *p)
{
    char buf[256];
    int n = avm_getstring(buf, (const acell *)avm_address(vm, p[1]),
                          sizeof buf);
    if (n > 0)
        write_all(1, buf, n);
    return 0;
}

static acell n_putint(avm *vm, acell *p)
{
    (void)vm;
    putint((int)p[1]);
    return 0;
}

static acell n_sleep(avm *vm, acell *p)
{
    (void)vm;
    return sleep((int)p[1]);
}

static acell n_uptime(avm *vm, acell *p)
{
    (void)vm;
    (void)p;
    return (acell)uptime();
}

static const avm_native_info natives[] = {
    { "tos_print", n_print },
    { "print", n_print },
    { "tos_putint", n_putint },
    { "tos_sleep", n_sleep },
    { "tos_uptime", n_uptime },
    { NULL, NULL }
};

extern const avm_native_info avm_extras_natives[];
int avm_script_argc;
char **avm_script_argv;
static avm_native_info all_natives[64];

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/bin/hello.amx";
    int fd, n;
    acell ret = 0;
    int rc;

    avm_script_argc = argc > 2 ? argc - 2 : 0;
    avm_script_argv = argv + 2;

    fd = open(path);
    if (fd < 0) {
        puts("avm: cannot open ");
        puts(path);
        puts("\n");
        return 2;
    }
    n = read(fd, block, sizeof block);
    close(fd);
    if (n <= 0) {
        puts("avm: cannot read ");
        puts(path);
        puts("\n");
        return 2;
    }

    int nn = 0;
    for (int i = 0; natives[i].name && nn < 63; i++)
        all_natives[nn++] = natives[i];
    for (int i = 0; avm_extras_natives[i].name && nn < 63; i++)
        all_natives[nn++] = avm_extras_natives[i];
    all_natives[nn].name = NULL;
    all_natives[nn].fn = NULL;
    rc = avm_init(&vm, block, (uint32_t)n, all_natives);
    if (rc != 0) {
        puts("avm: init ");
        putint(rc);
        if (vm.missing) {
            puts(" missing ");
            puts(vm.missing);
        }
        puts("\n");
        return 1;
    }

    rc = avm_run(&vm, 0xffffffffu, &ret);
    puts("avm rc=");
    putint(rc);
    puts(" ret=");
    putint((int)ret);
    puts("\n");
    return rc != 0;
}

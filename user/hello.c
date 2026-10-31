#include "lib.h"

int main(int argc, char *argv[])
{
    puts("hello from ring 3!\n");
    puts("pid=");
    putint(getpid());
    puts(" argc=");
    putint(argc);
    puts(" uptime=");
    putint(uptime());
    puts(" ms\n");
    for (int i = 0; i < argc; i++) {
        puts("  argv[");
        putint(i);
        puts("] = \"");
        puts(argv[i]);
        puts("\"\n");
    }
    return 0;
}

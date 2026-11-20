#include "lib.h"
#include "rvm.h"
#include "bench_code.h"
#include "sieve_code.h"

TOS_STACK(4 * 1024);

static RVM vm;

static void out_int(int v) { putint(v); }
static void out_chr(int c) { putchar(c); }
static uint32_t out_ms(void) { return (uint32_t)uptime(); }

int main(int argc, char **argv)
{
    const uint32_t *code = bench_code;
    uint32_t words = BENCH_CODE_WORDS;
    const char *name = "bench";
    uint32_t t0, t1;
    int rc;

    if (argc > 1 && argv[1][0] == 's') {
        code = sieve_code;
        words = SIEVE_CODE_WORDS;
        name = "sieve";
    }

    vm.putint = out_int;
    vm.putc = out_chr;
    vm.uptime = out_ms;

    t0 = (uint32_t)uptime();
    rc = rvm_run(&vm, code, words);
    t1 = (uint32_t)uptime();

    puts("rvm ");
    puts(name);
    puts(" rc=");
    putint(rc);
    puts(" ms=");
    putint((int)(t1 - t0));
    puts("\n");
    return rc != 0;
}

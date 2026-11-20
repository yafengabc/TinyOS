/* pvm: a minimal Pawn AMX runner for TinyOS.
 *
 * Usage: pvm <file.amx>
 *
 * The AMX program block is one contiguous buffer: header + code + data +
 * stack/heap, exactly as amx_Init() expects.  Scripts are compiled on the
 * host with pawncc; this program only initializes the VM, binds the TinyOS
 * natives and runs the script's main().
 */
#include "amxcfg.h"
#include "lib.h"
#include "fix16.h"
#include "amx.h"
#include <string.h>

/* One task window.  A script must fit header+code+data+stack in here; the
 * Pawn compiler's -S option sets the stack size that goes into the header,
 * so this is also the practical limit for a script's own stack and heap. */
#define PVM_BLOCK (8 * 1024)

TOS_STACK(4 * 1024);

static unsigned char block[PVM_BLOCK] __attribute__((aligned(8)));

static cell AMX_NATIVE_CALL n_print(AMX *amx, const cell *params)
{
    char buf[256];
    int len = 0;

    amx_StrLen(amx_Address(amx, params[1]), &len);
    if (len > (int)sizeof buf - 1)
        len = (int)sizeof buf - 1;
    amx_GetString(buf, amx_Address(amx, params[1]), 0, (size_t)len + 1);
    buf[len] = '\0';
    write_all(1, buf, len);
    return 0;
}

static cell AMX_NATIVE_CALL n_sleep(AMX *amx, const cell *params)
{
    (void)amx;
    return sleep((int)params[1]);
}

static cell AMX_NATIVE_CALL n_putint(AMX *amx, const cell *params)
{
    (void)amx;
    putint((int)params[1]);
    return 0;
}

static cell AMX_NATIVE_CALL n_uptime(AMX *amx, const cell *params)
{
    (void)amx;
    (void)params;
    return (cell)uptime();
}

/* csc strings are unpacked AMX strings (one cell per character), so these
 * use amx_GetString/amx_SetString and stay independent of Pawn's packed
 * representation. */
static cell AMX_NATIVE_CALL n_prints(AMX *amx, const cell *params)
{
    const char *s = (const char *)amx_Address(amx, params[1]);
    if (s)
        write_all(1, s, (int)strlen(s));
    return 0;
}

static cell AMX_NATIVE_CALL n_strlen(AMX *amx, const cell *params)
{
    return (cell)strlen((const char *)amx_Address(amx, params[1]));
}

static cell AMX_NATIVE_CALL n_strcmp(AMX *amx, const cell *params)
{
    return (cell)strcmp((const char *)amx_Address(amx, params[1]),
                        (const char *)amx_Address(amx, params[2]));
}

static cell AMX_NATIVE_CALL n_strlcpy(AMX *amx, const cell *params)
{
    char *dst = (char *)amx_Address(amx, params[1]);
    int cap = (int)params[2];
    const char *src = (const char *)amx_Address(amx, params[3]);
    int len = (int)strlen(src);
    if (cap > 0) {
        int n = len < cap - 1 ? len : cap - 1;
        memcpy(dst, src, (size_t)n);
        dst[n] = 0;
    }
    return (cell)len;
}

static cell AMX_NATIVE_CALL n_strlcat(AMX *amx, const cell *params)
{
    char *dst = (char *)amx_Address(amx, params[1]);
    int cap = (int)params[2];
    const char *src = (const char *)amx_Address(amx, params[3]);
    int dl = (int)strlen(dst);
    int sl = (int)strlen(src);
    if (cap > dl + 1) {
        int space = cap - 1 - dl;
        int copy = sl < space ? sl : space;
        memcpy(dst + dl, src, (size_t)copy);
        dst[dl + copy] = 0;
    }
    return (cell)(dl + sl);
}

static cell AMX_NATIVE_CALL n_atoi(AMX *amx, const cell *params)
{
    return (cell)atoi((const char *)amx_Address(amx, params[1]));
}

/* Q16.16 helpers for csc's source-level `float`. */
static cell AMX_NATIVE_CALL n_fix16_mul(AMX *amx, const cell *params)
{
    (void)amx;
    return (cell)fix16_mul((fix16_t)params[1], (fix16_t)params[2]);
}

static cell AMX_NATIVE_CALL n_fix16_div(AMX *amx, const cell *params)
{
    (void)amx;
    return (cell)fix16_div((fix16_t)params[1], (fix16_t)params[2]);
}

static cell AMX_NATIVE_CALL n_fix16_toint(AMX *amx, const cell *params)
{
    (void)amx;
    return (cell)fix16_to_int((fix16_t)params[1]);
}

static cell AMX_NATIVE_CALL n_fix16_print(AMX *amx, const cell *params)
{
    (void)amx;
    return (cell)printfix16((fix16_t)params[1]);
}

static cell AMX_NATIVE_CALL n_fix16_printn(AMX *amx, const cell *params)
{
    (void)amx;
    return (cell)printfix16_n((fix16_t)params[1], (int)params[2]);
}

/* Thin wrappers for the cheap, non-struct system calls.  Buffers are AMX
 * data-segment addresses (byte arrays), paths are byte strings. */
static cell AMX_NATIVE_CALL n_open(AMX *amx, const cell *params)
{
    return (cell)open((const char *)amx_Address(amx, params[1]));
}

static cell AMX_NATIVE_CALL n_create(AMX *amx, const cell *params)
{
    return (cell)create((const char *)amx_Address(amx, params[1]));
}

static cell AMX_NATIVE_CALL n_read(AMX *amx, const cell *params)
{
    return (cell)read((int)params[1], amx_Address(amx, params[2]),
                      (int)params[3]);
}

static cell AMX_NATIVE_CALL n_write(AMX *amx, const cell *params)
{
    return (cell)write((int)params[1], amx_Address(amx, params[2]),
                       (int)params[3]);
}

static cell AMX_NATIVE_CALL n_write_all(AMX *amx, const cell *params)
{
    return (cell)write_all((int)params[1], amx_Address(amx, params[2]),
                           (int)params[3]);
}

static cell AMX_NATIVE_CALL n_close(AMX *amx, const cell *params)
{
    (void)amx;
    return (cell)close((int)params[1]);
}

static cell AMX_NATIVE_CALL n_lseek(AMX *amx, const cell *params)
{
    (void)amx;
    return (cell)lseek((int)params[1], (int)params[2], (int)params[3]);
}

static cell AMX_NATIVE_CALL n_unlink(AMX *amx, const cell *params)
{
    return (cell)unlink((const char *)amx_Address(amx, params[1]));
}

static cell AMX_NATIVE_CALL n_chdir(AMX *amx, const cell *params)
{
    return (cell)chdir((const char *)amx_Address(amx, params[1]));
}

static cell AMX_NATIVE_CALL n_getcwd(AMX *amx, const cell *params)
{
    return (cell)getcwd((char *)amx_Address(amx, params[1]),
                        (int)params[2]);
}

static cell AMX_NATIVE_CALL n_readdir_index(AMX *amx, const cell *params)
{
    return (cell)readdir_index((int)params[1],
                               (char *)amx_Address(amx, params[2]),
                               (int)params[3]);
}

static cell AMX_NATIVE_CALL n_readdir_path(AMX *amx, const cell *params)
{
    return (cell)readdir_path((const char *)amx_Address(amx, params[1]),
                              (int)params[2],
                              (char *)amx_Address(amx, params[3]),
                              (int)params[4]);
}

static cell AMX_NATIVE_CALL n_getpid(AMX *amx, const cell *params)
{
    (void)amx;
    (void)params;
    return (cell)getpid();
}

static cell AMX_NATIVE_CALL n_yield(AMX *amx, const cell *params)
{
    (void)amx;
    (void)params;
    (void)yield();                 /* SYS_YIELD's raw return is not 0 */
    return 0;
}

static cell AMX_NATIVE_CALL n_dup(AMX *amx, const cell *params)
{
    (void)amx;
    return (cell)dup((int)params[1]);
}

static cell AMX_NATIVE_CALL n_ioctl(AMX *amx, const cell *params)
{
    (void)amx;
    return (cell)ioctl((int)params[1], (int)params[2], (int)params[3]);
}

static const AMX_NATIVE_INFO natives[] = {
    { "tos_print", n_print },
    { "print", n_print },
    { "tos_prints", n_prints },
    { "tos_sleep", n_sleep },
    { "tos_putint", n_putint },
    { "tos_uptime", n_uptime },
    { "tos_open", n_open },
    { "tos_create", n_create },
    { "tos_read", n_read },
    { "tos_write", n_write },
    { "tos_write_all", n_write_all },
    { "tos_close", n_close },
    { "tos_lseek", n_lseek },
    { "tos_unlink", n_unlink },
    { "tos_chdir", n_chdir },
    { "tos_getcwd", n_getcwd },
    { "tos_readdir_index", n_readdir_index },
    { "tos_readdir_path", n_readdir_path },
    { "tos_getpid", n_getpid },
    { "tos_yield", n_yield },
    { "tos_dup", n_dup },
    { "tos_ioctl", n_ioctl },
    { "tos_strlen", n_strlen },
    { "tos_strcmp", n_strcmp },
    { "tos_strlcpy", n_strlcpy },
    { "tos_strlcat", n_strlcat },
    { "tos_atoi", n_atoi },
    { "tos_fix16_mul", n_fix16_mul },
    { "tos_fix16_div", n_fix16_div },
    { "tos_fix16_toint", n_fix16_toint },
    { "tos_fix16_print", n_fix16_print },
    { "tos_fix16_printn", n_fix16_printn },
    { NULL, NULL }
};

extern const AMX_NATIVE_INFO pvm_hw_natives[];
#if defined(TOS_PVM_FLOAT)
extern const AMX_NATIVE_INFO pvm_extras_natives[];
#endif
int pvm_script_argc;
char **pvm_script_argv;
static AMX_NATIVE_INFO all_natives[64];

int main(int argc, char **argv)
{
    int fd, n, total = 0;
    AMX amx;
    AMX_HEADER *hdr;
    cell ret;
    int err;

    if (argc < 2) {
        puts("usage: pvm <file.amx> [args...]\n");
        return 2;
    }
    pvm_script_argc = argc > 2 ? argc - 2 : 0;
    pvm_script_argv = argv + 2;
    fd = open(argv[1]);
    if (fd < 0) {
        puts("pvm: cannot open ");
        puts(argv[1]);
        puts("\n");
        return 2;
    }
    while (total < PVM_BLOCK &&
           (n = read(fd, block + total, PVM_BLOCK - total)) > 0)
        total += n;
    close(fd);
    if (total <= 0) {
        puts("pvm: empty file\n");
        return 2;
    }
    if (total == PVM_BLOCK) {
        puts("pvm: program too big\n");
        return 2;
    }

    hdr = (AMX_HEADER *)block;
    if (hdr->magic != AMX_MAGIC_32) {
        puts("pvm: not a 32-bit AMX file\n");
        return 2;
    }
    if (hdr->stp > PVM_BLOCK) {
        puts("pvm: stack/heap does not fit the window\n");
        return 2;
    }

    /* amx_Init() expects the whole block; the tail past the file is the
     * script's stack and heap and starts zeroed. */
    memset(block + total, 0, PVM_BLOCK - total);

    memset(&amx, 0, sizeof amx);
    err = amx_Init(&amx, block);
    if (err) {
        puts("pvm: init ");
        putint(err);
        puts("\n");
        return 1;
    }
    int nn = 0;
    for (int i = 0; natives[i].name && nn < 63; i++)
        all_natives[nn++] = natives[i];
    for (int i = 0; pvm_hw_natives[i].name && nn < 63; i++)
        all_natives[nn++] = pvm_hw_natives[i];
#if defined(TOS_PVM_FLOAT)
    for (int i = 0; pvm_extras_natives[i].name && nn < 63; i++)
        all_natives[nn++] = pvm_extras_natives[i];
#endif
    all_natives[nn].name = NULL;
    all_natives[nn].func = NULL;
    err = amx_Register(&amx, all_natives, -1);
    if (err) {
        puts("pvm: register ");
        putint(err);
        puts("\n");
        return 1;
    }
    err = amx_Exec(&amx, &ret, AMX_EXEC_MAIN);
    if (err) {
        puts("pvm: exec ");
        putint(err);
        puts("\n");
        return 1;
    }
    return (int)ret;
}

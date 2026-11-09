/* libc/soft-float/printf shims for the Espressif WiFi blobs.
 *
 * These are ABI-level symbols the blobs were linked against in ESP-IDF.
 * Scan-only does not exercise the beacon-offset floating point path, so the
 * float helpers are deliberate zero stubs for now; the integer helpers are
 * real because littlefs/exec may share them.
 */
#include "types.h"
#include "heap.h"
#include "console.h"
#include "string.h"

/* ---- libc ------------------------------------------------------------ */

extern void *wifi_dma_ptr(void *p);
extern void *wifi_cpu_ptr(void *p);

void *malloc(size_t n) { return wifi_dma_ptr(kmalloc(n)); }
void free(void *p) { kfree(wifi_cpu_ptr(p)); }
void *calloc(size_t n, size_t s)
{
    void *p = kmalloc(n * s);

    if (p)
        memset(p, 0, n * s);
    return wifi_dma_ptr(p);
}
void *realloc(void *p, size_t n) { return wifi_dma_ptr(krealloc(wifi_cpu_ptr(p), n)); }

void puts(const char *s) { kprintf("%s", s); }

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

const char *WIFI_EVENT = "WIFI_EVENT";

int sprintf(char *buf, const char *fmt, ...)
{
    __builtin_va_list ap;
    int n;

    __builtin_va_start(ap, fmt);
    n = kvsnprintf(buf, 256, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

int __ctzsi2(unsigned x)
{
    int n = 0;

    if (!x)
        return 32;
    while (!(x & 1)) {
        x >>= 1;
        n++;
    }
    return n;
}

unsigned __bswapsi2(unsigned x)
{
    return ((x & 0x000000ffu) << 24) | ((x & 0x0000ff00u) << 8) |
           ((x & 0x00ff0000u) >> 8) | ((x & 0xff000000u) >> 24);
}

int hexstr2bin(const char *hex, uint8_t *buf, int len)
{
    int n = 0;

    (void)hex;
    for (int i = 0; i < len; i++)
        buf[i] = 0;
    return n;
}

/* ---- printf hooks ---------------------------------------------------- */

int32_t mesh_sta_auth_expire_time;

static void printv(const char *fmt, __builtin_va_list ap)
{
    char buf[192];

    kvsnprintf(buf, sizeof(buf), fmt, ap);
    kprintf("%s", buf);
}

void net80211_printf(const char *fmt, ...)
{
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    printv(fmt, ap);
    __builtin_va_end(ap);
}

void pp_printf(const char *fmt, ...)
{
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    printv(fmt, ap);
    __builtin_va_end(ap);
}

void phy_printf(const char *fmt, ...)
{
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    printv(fmt, ap);
    __builtin_va_end(ap);
}

/* ---- 64-bit integer helpers ----------------------------------------- */

int64_t __divdi3(int64_t a, int64_t b)
{
    int neg = (a < 0) ^ (b < 0);

    if (a < 0)
        a = -a;
    if (b < 0)
        b = -b;
    uint64_t q = (uint64_t)a / (uint64_t)b;
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t a, int64_t b)
{
    if (a < 0)
        a = -a;
    if (b < 0)
        b = -b;
    uint64_t r = (uint64_t)a % (uint64_t)b;
    return (int64_t)r;
}

/* ---- soft-float stubs ------------------------------------------------ *
 * The blobs are soft-float (ilp32).  The beacon-offset estimator is the
 * only user of these; scan does not depend on it.  Returning zero avoids
 * pulling compiler-rt for the PoC. */

float __addsf3(float a, float b) { (void)a; (void)b; return 0.0f; }
float __subsf3(float a, float b) { (void)a; (void)b; return 0.0f; }
float __mulsf3(float a, float b) { (void)a; (void)b; return 0.0f; }
float __divsf3(float a, float b) { (void)a; (void)b; return 0.0f; }
int   __gtsf2(float a, float b) { (void)a; (void)b; return 0; }
int   __ltsf2(float a, float b) { (void)a; (void)b; return 0; }
int   __nesf2(float a, float b) { (void)a; (void)b; return 0; }
int   __eqsf2(float a, float b) { (void)a; (void)b; return 1; }
float __floatsisf(int a) { (void)a; return 0.0f; }
float __floatunsisf(unsigned a) { (void)a; return 0.0f; }
float __floatundisf(unsigned long long a) { (void)a; return 0.0f; }
double __extendsfdf2(float a) { (void)a; return 0.0; }
double __floatsidf(int a) { (void)a; return 0.0; }
double __adddf3(double a, double b) { (void)a; (void)b; return 0.0; }
double __subdf3(double a, double b) { (void)a; (void)b; return 0.0; }
double __muldf3(double a, double b) { (void)a; (void)b; return 0.0; }
double __divdf3(double a, double b) { (void)a; (void)b; return 0.0; }
float __truncdfsf2(double a) { (void)a; return 0.0f; }
int __fixdfsi(double a) { (void)a; return 0; }

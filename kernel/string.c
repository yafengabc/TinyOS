#include "string.h"

/* Word-at-a-time copy.  This is not micro-optimisation for its own sake:
 * every filesystem block passes through memcpy() three times (littlefs cache
 * -> bounce buffer -> caller), and a listing of the boot filesystem copies
 * ~0.9 MB through it.  Byte-wise that is roughly six instructions per byte,
 * which turned a 0.2 s boot into half a minute on rv32 and would be far worse
 * on a 12 MHz RP2040.
 *
 * The word loop only runs while the two pointers keep the same alignment:
 * then peeling bytes until the destination is aligned leaves the source
 * aligned too, so no unaligned word access is ever performed.  (ARMv6-M and
 * some rv32 cores either fault or take a trap on those.)  Mixed alignment
 * falls back to the byte loop - it is rare here, and a shift-copy would cost
 * more code than it saves. */
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (((uintptr_t)d & (sizeof(uintptr_t) - 1)) ==
        ((uintptr_t)s & (sizeof(uintptr_t) - 1))) {
        while (n && ((uintptr_t)d & (sizeof(uintptr_t) - 1))) {
            *d++ = *s++;
            n--;
        }
        uintptr_t *dw = (uintptr_t *)d;
        const uintptr_t *sw = (const uintptr_t *)s;
        while (n >= sizeof(uintptr_t)) {
            *dw++ = *sw++;
            n -= sizeof(uintptr_t);
        }
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
    }

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

/* Same reasoning as memcpy(): block erases fill whole sectors with 0xFF and
 * littlefs zeroes 4 KiB buffers, so the word loop pays for itself at once.
 * The fill value is replicated across the word by multiplying by 0x01010101,
 * which keeps the code independent of pointer width. */
void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint8_t v = (uint8_t)c;

    while (n && ((uintptr_t)d & (sizeof(uintptr_t) - 1))) {
        *d++ = v;
        n--;
    }

    uintptr_t w = (uintptr_t)v * (uintptr_t)0x01010101u;
    uintptr_t *dw = (uintptr_t *)d;
    while (n >= sizeof(uintptr_t)) {
        *dw++ = w;
        n -= sizeof(uintptr_t);
    }
    d = (uint8_t *)dw;

    while (n--)
        *d++ = v;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (!n)
        return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    while (n--)
        *d++ = 0;
    return dst;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

int atoi(const char *s)
{
    int v = 0;
    int neg = 0;
    while (*s == ' ')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

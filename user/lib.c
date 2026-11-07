#include "lib.h"

/* Write everything or report why not.
 *
 * write() on a pipe returns a short count as soon as the ring is full - it
 * is a stream, not a message queue - and a program that ignores the return
 * value silently loses the tail of whatever it printed.  `cat demo.py | wc`
 * undercounted by a different amount on every run because of this: 2788
 * bytes one time, 2660 the next, 2532 the time after (OPEN-ITEMS §24).
 * Everything here that writes more than one byte goes through this. */
int write_all(int fd, const void *buf, int len)
{
    const char *p = buf;
    int done = 0;

    while (done < len) {
        int n = write(fd, p + done, len - done);

        if (n <= 0)
            return n < 0 ? n : done;    /* error, or the reader hung up */
        done += n;
    }
    return done;
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
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

/* The functions below are the ones include/string.h promises but used to
 * live only in kernel/string.c, which user programs do not link.  A user
 * program that included <string.h> and called memcmp() got an undefined
 * symbol at link time - the header said the function was there and the
 * linker said otherwise.  They are here so the header is true for user
 * land too.  --gc-sections drops the ones a program does not use, so this
 * costs nothing in a binary that does not call them. */

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
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            break;
    }
    return (char *)last;
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

char *strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return (char *)hay;
    for (; *hay; hay++) {
        if (*hay == *needle && !strncmp(hay, needle, n))
            return (char *)hay;
    }
    return NULL;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

/* Overlapping moves: the editor shifts the whole buffer left and right by a
 * few bytes when a character is typed, so `dst > src` is the normal case
 * and memcpy's direction is wrong. */
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
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

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a;
    const unsigned char *y = b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

int atoi(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* ------------------------------------------------------------------ */

static char outbuf[512];

/* The formatter behind printf().  Exported because a program that has to
 * put text in a buffer - to draw it on a panel, to send it as a file - has
 * no other way to do it: the lib deliberately carries no stdio. */
int tos_vsnprintf(char *buf, int cap, const char *fmt, __builtin_va_list ap)
{
    int n = 0;
#define PUT(ch)                     \
    do {                            \
        if (n + 1 < cap)            \
            buf[n] = (ch);          \
        n++;                        \
    } while (0)

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            PUT(*fmt);
            continue;
        }
        fmt++;
        int width = 0;
        char pad = ' ';
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        char tmp[16];
        if (*fmt == 'd' || *fmt == 'i') {
            int v = __builtin_va_arg(ap, int);
            unsigned int u = v < 0 ? (unsigned int)-v : (unsigned int)v;
            int k = 0;
            if (u == 0)
                tmp[k++] = '0';
            while (u) {
                tmp[k++] = (char)('0' + u % 10);
                u /= 10;
            }
            if (v < 0)
                PUT('-');
            while (width-- > k)
                PUT(pad);
            while (k--)
                PUT(tmp[k]);
        } else if (*fmt == 'u' || *fmt == 'x') {
            unsigned int v = __builtin_va_arg(ap, unsigned int);
            unsigned int base = (*fmt == 'x') ? 16 : 10;
            int k = 0;
            if (v == 0)
                tmp[k++] = '0';
            while (v) {
                unsigned int d = v % base;
                tmp[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                v /= base;
            }
            while (width-- > k)
                PUT(pad);
            while (k--)
                PUT(tmp[k]);
        } else if (*fmt == 'c') {
            PUT((char)__builtin_va_arg(ap, int));
        } else if (*fmt == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            while (s && *s)
                PUT(*s++);
        } else if (*fmt == '%') {
            PUT('%');
        } else {
            PUT('%');
            PUT(*fmt);
        }
    }
#undef PUT
    if (cap)
        buf[n < cap ? n : cap - 1] = '\0';
    return n;
}

int printf(const char *fmtstr, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmtstr);
    int n = tos_vsnprintf(outbuf, sizeof(outbuf), fmtstr, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(outbuf) - 1)
        n = sizeof(outbuf) - 1;
    write_all(1, outbuf, n);
    return n;
}

void puts(const char *s)
{
    write_all(1, s, (int)strlen(s));
}

void putchar(int c)
{
    char ch = (char)c;
    write(1, &ch, 1);
}

/* Print a signed decimal integer.  Builds the digits in a small stack
   buffer and writes them with one syscall, so it needs no static storage
   and costs ~70 bytes instead of printf's ~960.  Programs that only need
   plain %d output should prefer this over printf. */
int putint(int v)
{
    char t[12];
    int k = 0;
    unsigned int u = v < 0 ? (unsigned int)-v : (unsigned int)v;

    if (!u)
        t[k++] = '0';
    while (u) {
        t[k++] = (char)('0' + u % 10);
        u /= 10;
    }
    if (v < 0)
        t[k++] = '-';

    for (int i = 0, j = k - 1; i < j; i++, j--) {
        char c = t[i];
        t[i] = t[j];
        t[j] = c;
    }
    write_all(1, t, k);
    return k;
}

int getline(char *buf, int max)
{
    int n = read(0, buf, max - 1);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return n;
}

static unsigned int heap_ptr;
static unsigned int heap_end;

void *malloc(size_t n)
{
    n = (n + 7) & ~7u;
    if (!heap_ptr) {
        heap_ptr = (unsigned int)sbrk(0);
        heap_end = heap_ptr;
    }
    if (heap_ptr + n > heap_end) {
        int grow = (int)(((n + 4095) & ~4095u));
        void *p = sbrk(grow);
        if (!p || (unsigned int)p == 0xFFFFFFFFu)
            return 0;
        heap_end += grow;
    }
    void *p = (void *)heap_ptr;
    heap_ptr += n;
    return p;
}

void free(void *p)
{
    (void)p; /* bump allocator, nothing to do */
}

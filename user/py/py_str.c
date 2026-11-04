/* py_str.c - the two string helpers libc has no answer for.
 *
 * Splitting in place and joining pieces are the only string operations the
 * interpreter needs that the C library does not provide.  Everything else
 * comes from user/lib.c. */

#include "py_str.h"
#include "py_mem.h"

int py_split(char *s, char sep, char **out, int max)
{
    int n = 0;
    if (!s || max <= 0)
        return 0;
    out[n++] = s;
    for (char *p = s; *p && n < max; p++) {
        if (*p == sep) {
            *p = 0;
            out[n++] = p + 1;
        }
    }
    return n;
}

char *py_join(char **parts, int n, const char *sep, uint32_t *len_out)
{
    uint32_t total = 0;
    uint32_t sep_len = (uint32_t)strlen(sep);

    for (int i = 0; i < n; i++)
        total += (uint32_t)strlen(parts[i]);
    if (n > 1)
        total += sep_len * (uint32_t)(n - 1);

    char *out = (char *)py_alloc(total + 1);
    if (!out) {
        if (len_out)
            *len_out = 0;
        return 0;
    }
    char *w = out;
    for (int i = 0; i < n; i++) {
        uint32_t l = (uint32_t)strlen(parts[i]);
        memcpy(w, parts[i], l);
        w += l;
        if (i + 1 < n && sep_len) {
            memcpy(w, sep, sep_len);
            w += sep_len;
        }
    }
    *w = 0;
    if (len_out)
        *len_out = total;
    return out;
}

/* py_str.h - the string helpers the interpreter needs on top of libc.
 *
 * Everything the C library already has is used from there: strlen, strcmp,
 * strncmp, strcpy, memcpy, memcmp, memset, strchr, strrchr, strstr.  The
 * user libc (user/lib.c) grew the ones that were missing - memcmp, strchr,
 * strrchr, strncpy, strspn, strcspn, strstr - so that the promises
 * include/string.h makes are true for user land as well; before that, a
 * user program that included <string.h> and called memcmp() failed to link
 * against the kernel's copy.
 *
 * What is left here is the two helpers libc has no answer for: splitting a
 * string on a separator in place, and joining pieces back together.
 */

#ifndef TINYOS_PY_STR_H
#define TINYOS_PY_STR_H

#include "types.h"
#include "lib.h"

/* Split `s` on `sep`, writing up to `max` pieces into `out` (each a
 * pointer into `s`, NUL terminated in place).  Returns the number of
 * pieces found. */
int py_split(char *s, char sep, char **out, int max);

/* Join `n` pieces with `sep` between them.  The result is allocated from
 * the interpreter's arena and must be freed with py_free_auto(). */
char *py_join(char **parts, int n, const char *sep, uint32_t *len_out);

#endif

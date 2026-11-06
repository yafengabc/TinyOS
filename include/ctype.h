#ifndef TINYOS_STD_CTYPE_H
#define TINYOS_STD_CTYPE_H

/* Minimal freestanding ctype.h.  TinyUSB's debug formatter pulls it in to
 * decide whether a byte is printable; the inline versions below cover the
 * whole C locale and cost nothing when unused. */

#include "types.h"

static inline int isdigit(int c)  { return c >= '0' && c <= '9'; }
static inline int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c)  { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c)  { return isupper(c) || islower(c); }
static inline int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int isprint(int c)  { return c >= 0x20 && c < 0x7f; }
static inline int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline int tolower(int c)  { return isupper(c) ? c + ('a' - 'A') : c; }
static inline int toupper(int c)  { return islower(c) ? c - ('a' - 'A') : c; }

#endif

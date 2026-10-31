#ifndef TINYOS_CONSOLE_H
#define TINYOS_CONSOLE_H

#include "types.h"

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_set_color(uint8_t fg, uint8_t bg);

void kprintf(const char *fmt, ...);
int  ksnprintf(char *buf, size_t size, const char *fmt, ...);
int  kvsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
void panic(const char *fmt, ...) __attribute__((noreturn));

#endif

#ifndef TINYOS_CONSOLE_H
#define TINYOS_CONSOLE_H

#include "types.h"

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
/* Echo from the input side: printed whole, never inside a program's line. */
void console_echo(const char *s, size_t n);
void console_echo_tick(void);
void console_set_color(uint8_t fg, uint8_t bg);

/* Close the line the current task left open, if the console is in the
 * middle of somebody else's line.  A task preempted between two puts() must
 * not have its text continued by the next task's output or by the shell
 * prompt: kprintf() calls this before every kernel line, and so does the
 * scheduler before it prints anything of its own. */
void console_adopt_line(void);

/* Shutting down: drain the device and stop printing. */
void console_shutdown(void);

void kprintf(const char *fmt, ...);
int  ksnprintf(char *buf, size_t size, const char *fmt, ...);
int  kvsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
void panic(const char *fmt, ...) __attribute__((noreturn));

#endif

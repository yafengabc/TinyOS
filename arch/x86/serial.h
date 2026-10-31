#ifndef TINYOS_SERIAL_H
#define TINYOS_SERIAL_H

#include "types.h"

#define COM1 0x3F8

void serial_init(void);
void serial_poll(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);
int  serial_has_input(void);
int  serial_read(void);

#endif

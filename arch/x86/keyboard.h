#ifndef TINYOS_KEYBOARD_H
#define TINYOS_KEYBOARD_H

#include "types.h"

void keyboard_init(void);
void keyboard_irq(void);
int  keyboard_haschar(void);
int  keyboard_getchar(void);

#endif

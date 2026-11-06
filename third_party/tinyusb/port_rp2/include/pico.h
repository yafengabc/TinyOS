#ifndef _PICO_H
#define _PICO_H

/* TinyOS shim: the few pico-sdk facilities TinyUSB's RP2040 port expects to
 * get from "pico.h".  Everything here is either a compiler attribute that
 * becomes a no-op or a call into the TinyOS kernel, so the TinyUSB sources
 * stay byte-for-byte upstream. */

#include "types.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "pico/platform.h"

#endif

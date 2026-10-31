#ifndef TINYOS_STDBOOL_SHIM_H
#define TINYOS_STDBOOL_SHIM_H

/* Minimal freestanding stdbool.h for littlefs. */
#ifndef __cplusplus
#define bool   _Bool
#define true   1
#define false  0
#endif

#endif

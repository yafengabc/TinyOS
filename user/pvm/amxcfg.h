/* Build configuration for the vendored Pawn AMX core.
 *
 * The board build is freestanding: no dynamic library loading, no wchar_t
 * (TinyOS strings are UTF-8 bytes and Pawn can pass them through unpacked),
 * and 32-bit cells, which is what the AMX header on the wire uses.
 */
#ifndef TINYOS_PVM_AMXCFG_H
#define TINYOS_PVM_AMXCFG_H

#define AMX_NODYNALOAD
#define AMX_ANSIONLY
#define PAWN_CELL_SIZE 32

/* TinyOS only runs pawncc -O1 core P-code: no overlay, no packed opcodes,
 * no supplemental/macro instructions, and no amx_NativeInfo().  Compiling
 * them out removes the per-opcode packed mask and the unused switch cases. */
#define AMX_NO_OVERLAY
#define AMX_NO_PACKED_OPC
#define AMX_NO_MACRO_INSTR
#define AMX_NO_NATIVEINFO

/* amx.h pulls <alloca.h> when __GNUC__ is defined; TinyOS has no such
 * header, and the AMX core itself does not use alloca. */
#define HAVE_ALLOCA_H 0

/* osdefs.h falls back to <limits.h> for a path buffer size; TinyOS has no
 * limits.h and the dynamic loader (the only user of _MAX_PATH) is off. */
#define _MAX_PATH 64

#endif

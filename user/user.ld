ENTRY(_start)
SECTIONS
{
    /* Position independent: link near 0, the kernel relocates to APP_BASE.
       Using 0 for the image base keeps the bias arithmetic simple:
       bias = load_base - 0. */
    . = 0;

    .text : {
        *(.text.entry)
        *(.text*)
    }

    .rodata : ALIGN(16) {
        *(.rodata*)
    }

    /* What the program asks of the loader (TOS_STACK(), see user/lib.h): an
       ELF note, in read-only memory next to .rodata, and in a PT_NOTE
       segment of its own because the linker gives every allocated .note.*
       section one.  kernel/elf.c reads it out of the file before the task
       window is sized - the stack is part of that cost - so it never has to
       be reachable at run time.  Twenty-four bytes, and span is rounded to a
       page anyway. */
    .note.tos : ALIGN(4) {
        KEEP(*(.note.tos))
    }

    /* .data.rel.ro and relatives hold relocated pointers; keep them with
       writable data so the kernel can patch them after copying.  The
       32-byte alignment matters to the ARM port: it describes the
       read-only part of the image [win_base, wx) with power-of-two MPU
       regions whose smallest size is 32 bytes, so the split has to land on
       a 32-byte boundary or the last few bytes of code would fall into the
       non-executable window region.  The bare `. =` is what forces it when
       .data is empty and the next writable section takes the address. */
    . = ALIGN(32);
    .data : ALIGN(32) {
        *(.data.rel.ro*)
        *(.data*)
        *(.got*)
    }

    /* Dynamic relocation table, when the compiler emits one. */
    .dynamic : ALIGN(4) {
        *(.dynamic)
    }
    .rela.dyn : ALIGN(4) {
        *(.rela.dyn)
        *(.rela*)
    }
    .rel.dyn : ALIGN(4) {
        *(.rel.dyn)
        *(.rel*)
    }

    .bss : ALIGN(16) {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
        *(.interp)
        *(.gnu.hash)
        *(.hash)
        *(.dynsym)
        *(.dynstr)
    }
}

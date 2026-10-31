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

    /* .data.rel.ro and relatives hold relocated pointers; keep them with
       writable data so the kernel can patch them after copying. */
    .data : ALIGN(16) {
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

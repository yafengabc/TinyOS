    .section .text.entry, "ax"
    .globl _start
_start:
    cli

    /* clear .bss */
    cld
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    rep stosb

    movl $__kernel_stack_top, %esp
    xorl %ebp, %ebp
    call kmain

    cli
1:  hlt
    jmp 1b

    .section .bss
    .balign 16
    .globl __kernel_stack
__kernel_stack:
    .space 16384
    .globl __kernel_stack_top
__kernel_stack_top:

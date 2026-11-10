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

#ifndef TOS_KSTACK_BYTES
#define TOS_KSTACK_BYTES 32768
#endif
#define KSTACK_BYTES TOS_KSTACK_BYTES

    .section .bss
    .balign 16
    .globl __kernel_stack
__kernel_stack:
    /* Must equal KSTACK_BYTES * PLAT_MAX_CPUS (kernel/cpu.h).  This used to
     * be 16384 while cpu.h said 32768, and the only thing that noticed was
     * the stack guard: it is written at __kernel_stack_top - KSTACK_BYTES,
     * which on i386 landed 16 KiB below the stack - inside the process
     * table.  `ps` grew a phantom entry whose state was 0x5354414B ("STAK").
     * cpu_init() now checks the two against each other at boot. */
    .space KSTACK_BYTES
    .globl __kernel_stack_top
__kernel_stack_top:

    .section .text.entry, "ax"
    .globl _start
_start:
    xorl %ebp, %ebp
    movl (%esp), %eax          /* argc */
    leal 4(%esp), %edx         /* argv */
    pushl %edx
    pushl %eax
    call main
    addl $8, %esp
    pushl %eax
    call exit
1:  jmp 1b

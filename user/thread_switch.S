/*
 * Cooperative context switch, one implementation per architecture.
 *
 * void thread_switch(uint32_t *save_sp, uint32_t new_sp);
 *
 * Save the callee-saved registers (and the return address, which is not
 * callee-saved but is the only way back) on the current stack, write the
 * resulting sp through save_sp, then switch to new_sp and restore the same
 * set.  The frame layout here is the one user/thread.c's thread_frame()
 * builds for a brand new thread.
 */

#if defined(__riscv)

    .section .text.thread_switch, "ax"
    .globl thread_switch
thread_switch:
    addi    sp, sp, -64
    sw      ra,  0(sp)
    sw      s0,  4(sp)
    sw      s1,  8(sp)
    sw      s2, 12(sp)
    sw      s3, 16(sp)
    sw      s4, 20(sp)
    sw      s5, 24(sp)
    sw      s6, 28(sp)
    sw      s7, 32(sp)
    sw      s8, 36(sp)
    sw      s9, 40(sp)
    sw      s10, 44(sp)
    sw      s11, 48(sp)
    sw      sp, 0(a0)
    mv      sp, a1
    lw      ra,  0(sp)
    lw      s0,  4(sp)
    lw      s1,  8(sp)
    lw      s2, 12(sp)
    lw      s3, 16(sp)
    lw      s4, 20(sp)
    lw      s5, 24(sp)
    lw      s6, 28(sp)
    lw      s7, 32(sp)
    lw      s8, 36(sp)
    lw      s9, 40(sp)
    lw      s10, 44(sp)
    lw      s11, 48(sp)
    addi    sp, sp, 64
    ret

#elif defined(__i386__)

    .section .text.thread_switch, "ax"
    .globl thread_switch
thread_switch:
    movl    4(%esp), %edx           /* save_sp  */
    movl    8(%esp), %ecx           /* new_sp   */
    pushl   %ebp
    pushl   %ebx
    pushl   %esi
    pushl   %edi
    movl    %esp, (%edx)
    movl    %ecx, %esp
    popl    %edi
    popl    %esi
    popl    %ebx
    popl    %ebp
    ret

#elif defined(__arm__)

    .syntax unified
    .thumb
    .section .text.thread_switch, "ax"
    .globl thread_switch
    .thumb_func
thread_switch:
    push    {r4-r7, lr}
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    push    {r4-r7}
    mov     r4, r0
    mov     r5, r1
    mov     r0, sp
    str     r0, [r4]
    mov     sp, r5
    pop     {r4-r7}
    mov     r8, r4
    mov     r9, r5
    mov     r10, r6
    mov     r11, r7
    pop     {r4-r7, pc}

#endif

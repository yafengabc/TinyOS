/*
 * Interrupt/exception entry stubs.
 */
    .text

.macro ISR_NOERR num
    .globl isr\num
isr\num:
    pushl $0
    pushl $\num
    jmp isr_common
.endm

.macro ISR_ERR num
    .globl isr\num
isr\num:
    pushl $\num
    jmp isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

ISR_NOERR 128

    .globl isr_default
isr_default:
    pushl $0
    pushl $0xFF
    jmp isr_common

    .globl isr_common
isr_common:
    pusha

    xorl %eax, %eax
    movw %ds, %ax
    pushl %eax
    movw %es, %ax
    pushl %eax
    movw %fs, %ax
    pushl %eax
    movw %gs, %ax
    pushl %eax

    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushl %esp
    call isr_dispatch
    addl $4, %esp

    /* isr_dispatch returns the frame to resume.  Usually that is the one we
       just pushed, but a scheduler switch (or a syscall that blocked)
       hands back another task's frame, which lives in the process table.
       Everything below us on the kernel stack is dead either way: a trap
       from ring 3 always starts at the top of the kernel stack again. */
    movl %eax, %esp
    jmp .Lrestore

/*
 * void enter_user_frame(regs_t *frame)
 *
 * Turns a saved frame into a return to ring 3.  Never returns.
 */
    .globl enter_user_frame
enter_user_frame:
    /* Interrupts off before sp leaves the kernel stack: the frame handed in
       lives in the process table, and this routine is called from C with
       IF=1, so an IRQ landing between this and the iret would push its frame
       onto that table entry (the i386 shape of the rv32 bug fixed in
       arch/rv32/start.S).  iret reloads EFLAGS from the frame, so IF comes
       back on its own. */
    cli
    movl 4(%esp), %esp

.Lrestore:
    popl %eax
    movw %ax, %gs
    popl %eax
    movw %ax, %fs
    popl %eax
    movw %ax, %es
    popl %eax
    movw %ax, %ds

    popa
    addl $8, %esp
    iret

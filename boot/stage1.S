/*
 * TinyOS stage 1 boot sector.
 *
 * Responsibilities:
 *   - enable A20
 *   - load the flat kernel image (LBA 1 .. 180) to 0x00010000
 *   - switch to 32-bit protected mode and jump to the kernel entry
 *
 * The disk layout is fixed, so no patching of the boot sector is needed.
 * Everything has to fit into the first 512 bytes.
 *
 * The littlefs filesystem is NOT loaded here: it lives on the disk at LBA
 * 181 and the kernel reads it through its own ATA PIO driver once paging
 * is up.  This keeps the boot sector small and removes the real-mode DMA
 * window (0xA0000..0x100000) problem entirely.
 *
 * Note: the kernel image on disk only contains the file-backed part
 * (.text/.rodata/.data).  Its .bss is *not* stored; kernel/entry.S zeroes
 * __bss_start..__bss_end before calling kmain.  The boot loader therefore
 * reads a fixed number of sectors and the rest of the image is zero padding.
 */
#define KERNEL_LBA      1
#define KERNEL_SECTORS  180            /* 90 KiB: 0x10000 .. 0x2E000 */
#define KERNEL_SEG      0x1000         /* 0x1000 << 4 == 0x10000 */

    .section .text.entry, "ax"
    .code16
    .globl _start
_start:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7C00, %sp
    sti
    movb %dl, boot_drive

    /* Fast A20 gate.  Works on QEMU/SeaBIOS and virtually every PC made
       after 1995. */
    inb  $0x92, %al
    orb  $0x02, %al
    andb $0xFE, %al
    outb %al, $0x92

    /* We use LBA reads, make sure the BIOS supports them. */
    movw $0x55AA, %bx
    movb $0x41, %ah
    movb boot_drive, %dl
    int  $0x13
    jc   disk_error
    cmpw $0xAA55, %bx
    jne  disk_error

    /* kernel */
    movl $KERNEL_LBA, dap_lba
    movw $KERNEL_SEG, dap_seg
    movw $KERNEL_SECTORS, %ax
    call load_sectors

    cli
    lgdt gdt_desc
    movl %cr0, %eax
    orl  $0x00000001, %eax
    movl %eax, %cr0
    ljmp $0x08, $pm32

disk_error:
    movw $msg_disk, %si
    call print16
hang:
    cli
    hlt
    jmp hang

/* si -> NUL terminated string */
print16:
    lodsb
    testb %al, %al
    jz    1f
    movb  $0x0E, %ah
    movw  $0x0007, %bx
    int   $0x10
    jmp   print16
1:  ret

/*
 * Read AX sectors into DS:dap_seg starting at dap_lba.
 * Reads in chunks of at most 8 sectors (4 KiB) so that a transfer never
 * crosses a 64 KiB DMA boundary.
 */
load_sectors:
    movw %ax, sectors_left
1:
    movw sectors_left, %ax
    testw %ax, %ax
    jz    9f
    cmpw  $8, %ax
    jbe   2f
    movw  $8, %ax
2:
    movw %ax, dap_count
    movw $dap, %si
    movb boot_drive, %dl
    movb $0x42, %ah
    int  $0x13
    jc   disk_error

    movzwl dap_count, %ecx
    addl %ecx, dap_lba
    subw %cx, sectors_left

    shlw $5, %cx              /* count * 512 / 16 == count * 32 */
    addw %cx, dap_seg
    jmp  1b
9:
    ret

    .balign 4
dap:
    .byte 0x10, 0
dap_count:
    .word 0
dap_off:
    .word 0
dap_seg:
    .word 0
dap_lba:
    .long 0
    .long 0
sectors_left:
    .word 0
boot_drive:
    .byte 0
msg_disk:
    .asciz "TinyOS: disk read error\r\n"

    .balign 8
gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF      /* 0x08: flat 32-bit code, ring 0 */
    .quad 0x00CF92000000FFFF      /* 0x10: flat 32-bit data, ring 0 */
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt

    .code32
pm32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    movl $0x0009F000, %esp
    ljmp $0x08, $0x00010000

/* Can we bring up the C3's flash cache from our own code?
 *
 * Why this matters: the boot ROM loads our image into SRAM and never touches
 * the flash cache, so 0x42000000 (IROM) is dead when the kernel starts -
 * drom.S proved the same for the data window at 0x3C000000.  If the cache can
 * be enabled from kernel code, the kernel's .text/.rodata can be linked to
 * 0x42000000 and stop eating 63.7 KiB of SRAM.
 *
 * Two things the first version got wrong, both fixed here:
 *
 *  1. It did not disable the watchdogs, so a spin was reset after ~1 s and
 *     the capture looked like a crash at whatever step happened to run.  The
 *     kernel turns off TG0 WDT, the RTC WDT and the RTC super WDT before it
 *     does anything else (board/esp32c3/platform.c); this does the same.
 *  2. It printed only the result of a call, so a call that died left no
 *     trace.  Every step now prints a marker BEFORE it runs and its result
 *     after, so the last marker in the capture names the failing step.
 *
 * The cache API is the ROM's own (addresses from esp-idf's
 * components/esp_rom/esp32c3/ld/esp32c3.rom.ld, signatures from
 * .../esp32c3/rom/cache.h):
 *
 *   ROM_Boot_Cache_Init         0x400004c8  void (void)
 *       reset ICache, init owner + MMU, set mode, enable ICache, unmask bus
 *   Cache_Ibus_MMU_Set          0x40000560  int (ext_ram, vaddr, paddr,
 *                                                psize, num, fixed)
 *   Cache_Invalidate_ICache_All 0x400004d8  void (void)
 *   Cache_Get_IROM_MMU_End      0x4000054c  uint32_t (void)
 *
 * Output:
 *   W          alive
 *   1          watchdogs off
 *   2          cache init returned
 *   m          about to map
 *   B <rc>     Cache_Ibus_MMU_Set returned <rc> (0 = mapped)
 *   e          about to query
 *   E <hex32>  Cache_Get_IROM_MMU_End
 *   a          about to read IROM
 *   A <16 B>   flash offset 0 read through 0x42000000 (want: e9 ...)
 */

/* Flash offset the probe maps.  Overridable so one image can prove the
 * mapping is live: -DPADDR=0x40000 lands in an erased sector. */
#ifndef PADDR
#define PADDR 0
#endif
/* Virtual address the probe maps.  Default is the IROM window base; moving
 * it by a page tests whether entry 0 is special (the ROM keeps the app
 * partition mapped there at boot). */
#ifndef VADDR
#define VADDR 0x42000000
#endif

    .section .text
    .globl _start
_start:
    li   sp, 0x403DC000            /* top of usable SRAM, ROM stack is above */

    li   a0, 'W'
    call putc_nl

    /* 0. watchdogs off: TG0, RTC, and the RTC super watchdog.  Same keys and
     *    registers as the kernel, see board/esp32c3/platform.c. */
    li   t1, 0x50D83AA1
    li   t0, 0x6001F064            /* TIMG0 WDTWPROTECT */
    sw   t1, 0(t0)
    li   t0, 0x6001F048            /* TIMG0 WDTCONFIG0 */
    sw   zero, 0(t0)

    li   t0, 0x600080A8            /* RTC_CNTL WDT WPROTECT */
    sw   t1, 0(t0)
    li   t0, 0x60008090            /* RTC_CNTL WDT CONFIG0 */
    sw   zero, 0(t0)

    li   t2, 0x8F1D312A
    li   t0, 0x600080B0            /* RTC_CNTL SWD WPROTECT */
    sw   t2, 0(t0)
    li   t0, 0x600080AC            /* RTC_CNTL SWD CONF */
    li   t3, 0x40000000            /* SWD_DISABLE */
    sw   t3, 0(t0)

    li   a0, '1'
    call putc_nl

    /* 1. full cache bring-up, straight from the ROM.  MODE=4 skips it
     *    entirely, as a control: if the probe then prints all the way to the
     *    A line, the printing path is fine and the cache is what wedges.
     *    MODE=10 instead re-initialises the MMU from scratch: mark every
     *    entry invalid, set the I/D split, and only then write ours. */
.if MODE == 10
    li   t0, 0x4000055c            /* Cache_MMU_Init */
    jalr ra, t0, 0
    li   t0, 0x40000554            /* Cache_Owner_Init */
    jalr ra, t0, 0
    li   a0, 64
    li   a1, 0
    li   t0, 0x40000548            /* Cache_Set_IDROM_MMU_Size */
    jalr ra, t0, 0
    li   a0, 'q'
    call putc_nl
.endif
.if MODE != 4
    li   t0, 0x400004c8
    jalr ra, t0, 0
    li   a0, '2'
    call putc_nl
.if MODE == 7
    /* The routine esp-idf's own bootloader calls to switch XIP on: it sets
     * up the flash side (SPI0) as well as the cache, which Cache_*_MMU_Set
     * alone does not.  Argument is the cache size in bytes; 0 = default. */
    li   a0, 0
    li   t0, 0x400001d0
    jalr ra, t0, 0
    li   a0, 'p'
    call putc_nl
.endif
.else
    li   a0, 'n'
    call putc_nl
.endif

    /* 2. identity-map 8 x 64 KiB = 512 KiB of flash at 0x42000000, exactly
     *    the kernel slot the image is burned into (flash offset 0).
     *
     *    MODE=1 skips this and MODE=2 skips the invalidate, so a wedged bus
     *    can be bisected: assemble with -DMODE=1 / -DMODE=2. */
.if MODE != 1 && MODE != 4 && MODE != 15
    li   a0, 'm'
    call putc_nl
.if MODE == 8
    /* The cache must be off while the MMU entries are written: writing them
     * with it running is what left every read pointing at the ROM's own
     * mapping (flash 0x10000, the factory app partition). */
    li   t0, 0x4000051c            /* Cache_Disable_ICache */
    jalr ra, t0, 0
    li   a0, 'x'
    call putc_nl
.endif
.if MODE == 5
    /* How many of the 64 MMU entries belong to IROM vs DROM.  The readback
     * without this returned some other image's header, which says the entry
     * we wrote was not the one the IROM window reads through. */
    li   a0, 64
    li   a1, 0
    li   t0, 0x40000548
    jalr ra, t0, 0
    li   a0, 's'
    call putc_nl
.endif
    li   a0, 1                     /* ext_ram: flash */
    li   a1, VADDR                 /* vaddr */
    li   a2, PADDR                 /* paddr: flash offset */
    li   a3, 64                    /* psize: 64 KiB pages */
    li   a4, 8                     /* num pages */
    li   a5, 0                     /* fixed: 0 = identity */
    li   t0, 0x40000560
    jalr ra, t0, 0
    mv   s1, a0                    /* keep the return code */

    /* Same pages through the data bus, so a load can check the mapping:
     * 0x42000000 is the *fetch* window (Ibus), and a plain `lbu` from it
     * goes through the Dbus MMU instead - which is why the first version
     * read some unrelated image and looked like a failed mapping. */
    li   a0, 1
    li   a1, 0x3C000000            /* DROM window */
    li   a2, 0
    li   a3, 64
    li   a4, 8
    li   a5, 0
    li   t0, 0x40000564            /* Cache_Dbus_MMU_Set */
    jalr ra, t0, 0
    mv   s6, a0
.if MODE == 8
    li   t0, 0x40000520            /* Cache_Enable_ICache */
    jalr ra, t0, 0
    li   a0, 'y'
    call putc_nl
.endif
.else
    li   s1, 0xff
.endif

    /* 3. drop anything the cache may have picked up before the mapping */
.if MODE != 2 && MODE != 4
    li   t0, 0x400004d8
    jalr ra, t0, 0
.endif

    li   a0, 'B'
    call putc
    li   a0, ' '
    call putc
    mv   a0, s1
    call put_hex8
    li   a0, '\n'
    call putc

    li   a0, 'e'
    call putc_nl
    li   t0, 0x4000054c
    jalr ra, t0, 0
    call put_hex32
    li   a0, '\n'
    call putc


.if MODE == 12
    li   a0, 'C'
    call putc
    li   a0, ' '
    call putc
    li   a1, 0x600C8000
    call dump32
    li   a0, 'E'
    call putc
    li   a0, ' '
    call putc
    li   a1, 0x600C4000
    call dump32
    li   a0, 'S'
    call putc
    li   a0, ' '
    call putc
    li   a1, 0x3FC80000
    call dump32
.endif


.if MODE == 13
    /* Scan the 32 KiB CACHE block and print every non-zero word.  The MMU
     * table has to be in here somewhere: the ROM's mapping is live, so at
     * least a few entries must be non-zero. */
    li   s7, 0x600C4000
    li   s8, 0x600CC000
    li   s9, 0
2:  lw   t6, 0(s7)
    beqz t6, 3f
    mv   a0, s7
    call put_hex32
    li   a0, ':'
    call putc
    mv   a0, t6
    call put_hex32
    li   a0, ' '
    call putc
    addi s9, s9, 1
    li   t5, 24
    blt  s9, t5, 3f
    li   a0, '\n'
    call putc
    j    4f
3:  addi s7, s7, 4
    blt  s7, s8, 2b
4:  li   a0, '\n'
    call putc
.endif


.if MODE == 14
    /* Scan SRAM for a linear MMU table: three consecutive words 1,2,3.
     * The ROM maps flash 0x10000 at the window base, so entry 0 holds page
     * 1 and the next two hold 2 and 3 if the table is identity-mapped. */
    li   s7, 0x3FC80000
    li   s8, 0x3FCDC000
    li   s9, 0
2:  lw   t3, 0(s7)
    li   t4, 1
    bne  t3, t4, 3f
    lw   t3, 4(s7)
    li   t4, 2
    bne  t3, t4, 3f
    lw   t3, 8(s7)
    li   t4, 3
    bne  t3, t4, 3f
    mv   a0, s7
    call put_hex32
    li   a0, ' '
    call putc
    addi s9, s9, 1
    li   t5, 6
    bge  s9, t5, 4f
3:  addi s7, s7, 4
    blt  s7, s8, 2b
4:  li   a0, '\n'
    call putc
.endif


.if MODE == 15
    /* The MMU table itself, from esp32c3-hal: 0x600C5000, 4-byte entries,
     * invalid marked 0x100, 64 KiB per page.  Read what the ROM left, then
     * rewrite entry 0 to point at flash offset 0 and see if the window
     * follows. */
    li   a0, 't'
    call putc_nl
    li   s7, 0x600C5000
    mv   a1, s7
    call dump32

    li   t0, 0                    /* page 0 -> flash 0x00000, valid */
    sw   t0, 0(s7)
    li   t0, 0x10000              /* page 1 -> flash 0x10000 */
    sw   t0, 4(s7)

    li   t0, 0x400004d8           /* Cache_Invalidate_ICache_All */
    jalr ra, t0, 0

    li   a0, 'u'
    call putc_nl
    mv   a1, s7
    call dump32
.endif

    /* 4. the payoff: if this reads the image header, XIP works */
    li   a0, 'a'
    call putc_nl
    li   a0, 'A'
    call putc
    li   a0, ' '
    call putc
    li   a1, VADDR
    call dump16

    li   a0, 'D'
    call putc
    li   a0, ' '
    call putc
    li   a1, 0x3C000000
    call dump16

1:  j    1b

/* putc_nl(a0): the character, then a newline.  It must save ra: `j putc`
 * would return into this function and print newlines forever (which is
 * exactly what the first version did, and the watchdog reset it). */
putc_nl:
    addi sp, sp, -16
    sw   ra, 12(sp)
    call putc
    li   a0, '\n'
    call putc
    lw   ra, 12(sp)
    addi sp, sp, 16
    ret

/* dump16(a1): 16 bytes as "xx " */
dump16:
    addi sp, sp, -16
    sw   ra, 12(sp)
    mv   a2, a1
    li   a3, 16
1:  lbu  a0, 0(a2)
    call put_hex8
    li   a0, ' '
    call putc
    addi a2, a2, 1
    addi a3, a3, -1
    bnez a3, 1b
    li   a0, '\n'
    call putc
    lw   ra, 12(sp)
    addi sp, sp, 16
    ret

/* dump32(a1): eight 32-bit words as "xxxxxxxx " */
dump32:
    addi sp, sp, -16
    sw   ra, 12(sp)
    mv   a2, a1
    li   a3, 8
1:  lw   a0, 0(a2)
    call put_hex32
    li   a0, ' '
    call putc
    addi a2, a2, 4
    addi a3, a3, -1
    bnez a3, 1b
    li   a0, '\n'
    call putc
    lw   ra, 12(sp)
    addi sp, sp, 16
    ret

/* put_hex8(a0): two hex digits.  ra must be saved: the last `call nib`
 * leaves ra pointing at the instruction after it, so a bare `ret` here would
 * jump to itself and loop forever - which is exactly what made every earlier
 * probe stop right after the second hex digit.  The value also lives in a
 * saved register so the second digit is not computed from the first digit's
 * ASCII code (the bug in drom.S). */
put_hex8:
    addi sp, sp, -16
    sw   ra, 12(sp)
    mv   s4, a0
    srli s5, s4, 4
    andi s5, s5, 0xf
    mv   a0, s5
    call nib
    andi s5, s4, 0xf
    mv   a0, s5
    call nib
    lw   ra, 12(sp)
    addi sp, sp, 16
    ret

/* put_hex32(a0): eight hex digits, most significant first */
put_hex32:
    mv   s3, ra
    mv   s4, a0
    li   s2, 28
1:  srl  t5, s4, s2
    andi t5, t5, 0xf
    mv   a0, t5
    call nib
    addi s2, s2, -4
    bgez s2, 1b
    mv   ra, s3
    ret

/* nib(a0): print one hex digit for 0..15 */
nib:
    li   t6, 10
    li   t5, '0'
    bltu a0, t6, 1f
    addi t5, a0, 'a' - 10
    j    2f
1:  add  t5, t5, a0
2:  mv   a0, t5
    j    putc

/* putc: wait (bounded!) for room in the TX FIFO, write, then burn time so the
 * byte leaves before any reset.  The wait is bounded on purpose: an unbounded
 * spin on the FIFO count is what wedged the second version of this probe right
 * after "B 00", with no way to tell a stuck register from a dead CPU. */
putc:
    li   t1, 0x6000001c
    li   t4, 200000
1:  lw   t2, 0(t1)
    srli t2, t2, 16
    andi t2, t2, 0xff
    li   t3, 100
    bltu t2, t3, 2f
    addi t4, t4, -1
    bnez t4, 1b
2:  li   t1, 0x60000000
    sw   a0, 0(t1)
    li   t3, 2000
3:  addi t3, t3, -1
    bnez t3, 3b
    ret

    .balign 4

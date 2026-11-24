#include "platform.h"
#include "board_config.h"

/* Park-skeleton hooks (issue #17).  Defined before rp_park.h so the shared
 * RP_PARK_OWNER_TAKE / RP_PARK_PARKED_LOOP macros pick them up.  They name
 * symbols declared later in this file, which is fine: a macro is only expanded
 * at its call site, well after those declarations.  See board/rp_park.h. */
#define RP_PARK_WDT_TICK(spins)                                              \
    do { if (((spins) & 0xfffffu) == 0xfffffu) wdt_feed(); } while (0)
#define RP_PARK_STOP_FOREVER()                                               \
    do { if (g_core1_stopped) for (;;) wdt_feed(); } while (0)
#if TOS_DIAG
#define RP_PARK_PARKED_DIAG(entering)                                        \
    do {                                                                     \
        uint32_t _rp_d = (uint32_t)plat_cpu_id() & 1u;                       \
        if (entering) { g_park_phase = 2; g_park_in[_rp_d] = 1; }            \
        else { g_park_in[_rp_d] = 0; }                                       \
    } while (0)
#define RP_PARK_OWNER_PREPARE()                                              \
    do {                                                                     \
        uint32_t _rp_mid = (uint32_t)plat_cpu_id() & 1u;                     \
        g_mask_lr[_rp_mid]   = (uint32_t)__builtin_return_address(0);        \
        g_mask_site[_rp_mid] = TOS_MASKSITE_FLASH_PARK;                      \
        g_mask_seq[_rp_mid]++;                                               \
        g_mask_in[_rp_mid]   = 1;                                            \
        g_park_phase = 1;                                                    \
    } while (0)
#define RP_PARK_OWNER_TICK(spins)                                            \
    do {                                                                     \
        if (((spins) & 0xfffffu) != 0xfffffu)                                \
            break;                                                           \
        wdt_feed();                                                          \
        diag_char('?');                                                      \
        diag_str(" c1t=");                                                   \
        diag_u32(g_c1_ticks);                                                \
        diag_str(" idle=");                                                  \
        diag_u32(g_cpu_idle[1]);                                             \
        diag_str(" pk=");                                                    \
        diag_u32(g_park_phase);                                              \
        diag_str(" pn=");                                                    \
        diag_u32(g_park_poll_n);                                             \
        diag_str(" pf=");                                                    \
        diag_u32(g_park_poll_flag);                                          \
        diag_str(" pi=");                                                    \
        diag_u32(g_park_poll_id);                                            \
        diag_str(" po=");                                                    \
        diag_u32(g_park_poll_owner);                                         \
        diag_str(" myid=");                                                  \
        diag_u32((uint32_t)plat_cpu_id());                                   \
        diag_str(" myown=");                                                 \
        diag_u32(g_park_poll_owner);                                         \
        diag_str(" c0pc=");                                                  \
        diag_u32(g_tick_pc[0]);                                              \
        diag_str(" c0lr=");                                                  \
        diag_u32(g_tick_lr[0]);                                              \
        diag_str(" c1pc=");                                                  \
        diag_u32(g_tick_pc[1]);                                              \
        diag_str(" mseq0=");                                                 \
        diag_u32(g_mask_seq[0]);                                             \
        diag_str(" mlr0=");                                                  \
        diag_u32(g_mask_lr[0]);                                              \
        diag_str(" msite0=");                                                \
        diag_u32(g_mask_site[0]);                                            \
        diag_str(" isite=");                                                 \
        diag_u32(g_idle_site);                                               \
        diag_str(" us0=");                                                   \
        diag_u32(g_usb_site[0]);                                             \
        diag_str(" us1=");                                                   \
        diag_u32(g_usb_site[1]);                                             \
        diag_str("\r\n");                                                    \
    } while (0)
#else
#define RP_PARK_PARKED_DIAG(entering) do { (void)(entering); } while (0)
#define RP_PARK_OWNER_PREPARE() do { } while (0)
#define RP_PARK_OWNER_TICK(spins) RP_PARK_WDT_TICK(spins)
#endif

#include "rp_park.h"
#include "board.h"
#include "board_internal.h"
#include "arm_memmap.h"
#include "rp2350_xosc.h"   /* XOSC is a chip fact shared with the RV32 port */
#include "console.h"
#include "tty.h"
#include "usb_cdc.h"
#include "string.h"
#include "cpu.h"

/* Code that must run from SRAM: XIP is off inside the flash window, and an
 * instruction fetch then bus-faults.  The linker script copies
 * .data.ramfunc before kmain. */
#define RAMFUNC __attribute__((section(".data.ramfunc"), noinline))

/* Board: Raspberry Pi RP2350 (dual Cortex-M33, ARMv8-M, Secure state).
 *
 * Boot flow: the mask ROM configures XIP itself - there is no RP2040-style
 * 256-byte stage-2 - scans the first 4 KiB of flash for a PICOBIN IMAGE_DEF
 * block, then loads MSP/PC from the ARM vector table at 0x10000000.  Our
 * linker script puts .vectors at flash offset 0 and image_def.S right after
 * it.  The ROM hands over in Secure state (TrustZone is always enabled on
 * the RP2350); we never switch to Non-secure, so the S aliases of every
 * peripheral are the right ones to use.
 *
 * Console: two independent wires, both live at once.  UART0 on GPIO0/1
 * (115200 8N1, for a USB-TTL adapter) AND the on-chip USB device as a
 * CDC-ACM COM port (usb_cdc.c, polled by the tick, no USB interrupt).
 * Either cable alone is enough: plugging the board's own USB port into a PC
 * gives a shell with no extra hardware, and the same output is mirrored to
 * the UART for anyone watching with an adapter.  Input is accepted from both.
 *
 * Clock: the ROM leaves clk_sys on the ~12 MHz ROSC/XOSC path.  Bring-up
 * enables the crystal, pins clk_ref to it, brings PLL_SYS up at 150 MHz
 * (pll_sys_init) and re-pins clk_peri to clk_sys; the frequency counter
 * then *measures* clk_sys against the 12 MHz reference and both the UART
 * baud divisor and the SysTick reload come from that measurement, never
 * from the configured constant.  If the PLL refuses to lock the boot
 * simply continues at the ROM's clock - every divisor still comes out
 * right because it was measured, not assumed.
 *
 * Flash: reads go straight through XIP in the mode the boot ROM left.  The
 * kernel never writes QMI: measured on the Pico 2, the ROM's own EBh quad
 * timing still reads flash correctly after clk_sys moves to 150 MHz, and
 * re-timing it in the same step is what used to break XIP (issue #7).
 * Writes and erases use the RP2350
 * boot ROM's low-level flash API.  Its lookup ABI differs from RP2040: a
 * (code, flags) call through a 16-bit pointer at 0x16 with
 * RT_FLAG_FUNC_ARM_SEC.  Those wrappers live in .data.ramfunc (copied to
 * SRAM by _start) because QSPI goes into serial-command mode mid-call. */

/* ---- boot ROM flash API ------------------------------------------------ */

typedef void *(*rp_rom_lookup_t)(uint32_t code, uint32_t flags);
typedef void (*rp_rom_void_t)(void);
typedef void (*rp_rom_erase_t)(uint32_t addr, size_t count,
                               uint32_t block_size, uint8_t block_cmd);
typedef void (*rp_rom_program_t)(uint32_t addr, const uint8_t *data, size_t count);

#define ROM_FUNC_CONNECT_INTERNAL_FLASH 0x4649u   /* 'I','F' */
#define ROM_FUNC_FLASH_EXIT_XIP        0x5845u   /* 'E','X' */
#define ROM_FUNC_FLASH_ENTER_CMD_XIP   0x5843u   /* 'C','X' */
#define ROM_FUNC_FLASH_FLUSH_CACHE     0x4346u   /* 'F','C' */
#define ROM_FUNC_FLASH_RANGE_ERASE    0x4552u   /* 'R','E' */
#define ROM_FUNC_FLASH_RANGE_PROGRAM   0x5052u   /* 'R','P' */

/* QMI direct-mode registers: the only way to send an arbitrary flash
 * command (JEDEC RDID) - the ROM's FLASH_OP entry covers erase/program/
 * read only.  Layout per the RP2350 datasheet / pico-sdk structs/qmi.h,
 * base = XIP_QMI_BASE (0x400d0000).  In direct mode each byte written to
 * DIRECT_TX clocks one byte in on DIRECT_RX, so an N-byte command must be
 * read back N bytes; CSn is asserted by setting ASSERT_CS0N. */
#define QMI_DIRECT_CSR          (XIP_QMI_BASE + 0x00u)
#define QMI_DIRECT_TX           (XIP_QMI_BASE + 0x04u)
#define QMI_DIRECT_RX           (XIP_QMI_BASE + 0x08u)
#define QMI_DIRECT_CSR_EN           (1u << 0)
#define QMI_DIRECT_CSR_ASSERT_CS0N  (1u << 2)
#define QMI_DIRECT_CSR_TXFULL       (1u << 10)
#define QMI_DIRECT_CSR_RXEMPTY      (1u << 16)
/* CLKDIV[29:22]: SCK = clk_sys / (CLKDIV + 1).  It lives in this same
 * register and its reset value is 6, which is what the ROM leaves behind.
 * Writing the register without it sets CLKDIV=0 - SCK at clk_sys, 150 MHz on
 * this board - and the flash cannot answer at that rate, so RXEMPTY never
 * clears and both bounded waits below run their full 1e6 iterations.  That
 * stall (about 2 s, inside the window where XIP is off) is what made the
 * single-core ARM image reset forever in the watchdog (#42). */
#define QMI_DIRECT_CSR_CLKDIV_BITS  (0x3fu << 22)
#define QMI_DIRECT_CSR_CLKDIV_DEF   (6u << 22)

/* #42 diagnostic toggle: 1 restores the old "leave direct mode by writing 0"
 * exit, which is the single line the board experiments finger.  The BM arms
 * a test image with 0 and 1 and compares the two traces.  Default 0 is the
 * fixed behaviour and what ships. */
#ifndef QMI_DIAG_OLD_EXIT
#define QMI_DIAG_OLD_EXIT 0
#endif

#if TOS_BOOT_TRACE
/* The probe runs from SRAM inside the XIP-off window, so the printer is
 * RAM-resident and its text sits in .data (SRAM): a flash-resident
 * early_puts() would have to fetch the very code/data that a broken XIP
 * restore makes unreadable, and would hang before printing anything.  Only
 * the explicit XIP read at the end of the probe touches flash. */
static char qmi_diag_csr0[]  = "\r\n[Q] csr0=";
static char qmi_diag_clkdiv[] = " clkdiv=";
static char qmi_diag_exit[]  = "\r\n[Q] exit=";
static char qmi_diag_id[]    = " id=";
static char qmi_diag_spins[] = " spins=";
static char qmi_diag_csr1[]  = "\r\n[Q] csr1=";
static char qmi_diag_m0[]    = " m0=";
static char qmi_diag_xip[]   = " xip=";
static char qmi_diag_ok[]    = " ok";
static char qmi_diag_bad[]   = " MISMATCH";
static char qmi_diag_pre[]   = " pre=";
static char qmi_diag_post[]  = " post=";
static char qmi_diag_nl[]    = "\r\n";

static RAMFUNC void qmi_diag_putc(char c)
{
    while (mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF)
        ;
    mmio_write32(RP_UART_DR, (uint32_t)(unsigned char)c);
}

static RAMFUNC void qmi_diag_puts(const char *s)
{
    while (*s)
        qmi_diag_putc(*s++);
}

static RAMFUNC void qmi_diag_hex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        uint32_t d = (v >> i) & 0xfu;

        qmi_diag_putc(d < 10u ? (char)('0' + d) : (char)('a' + d - 10u));
    }
}
#endif /* TOS_BOOT_TRACE */


/* Data-table lookup ('F','D' = 0x4446, the bootram FLASH_DEVINFO16 copy).
 * The ROM's data table is addressed with the same (code, flags) lookup
 * entry as the function table, only the flag differs: RT_FLAG_DATA (0x40)
 * selects the data table, RT_FLAG_FUNC_ARM_SEC (0x4) the secure function
 * table.  On RP2350 the returned value for a data row is a pointer to a
 * 32-bit pointer literal in ROM, which itself points at the live copy in
 * bootram - so a data lookup needs one more dereference than a function
 * lookup.  (pico-sdk bootrom_constants.h + flash.c: flash_devinfo_ptr().) */
#define ROM_DATA_FLASH_DEVINFO16_PTR 0x4446u   /* 'F','D' */
#define ROM_DATA_FLAG_DATA           0x0040u

/* bootram FLASH_DEVINFO bit fields (OTP_DATA_FLASH_DEVINFO, RP2350 OTP):
 * bits 12:15 = CS1_SIZE, bits 8:11 = CS0_SIZE, bit 7 = D8H_ERASE_SUPPORTED.
 * The capacity of the primary CS0 flash is 4096u << CS0_SIZE - the exact
 * decode pico-sdk's flash_devinfo_size_to_bytes() applies.  With the OTP
 * BOOT_FLAGS0_FLASH_DEVINFO_ENABLE bit not set the ROM reports the
 * conservative default CS0_SIZE = 12 (16 MiB). */
#define FLASH_DEVINFO_CS0_SIZE_MASK  0x0f00u
#define FLASH_DEVINFO_CS0_SIZE_LSB   8

static rp_rom_void_t    g_rom_connect;
static rp_rom_void_t    g_rom_exit_xip;
static rp_rom_void_t    g_rom_enter_xip;
static rp_rom_void_t    g_rom_flush;
static rp_rom_erase_t   g_rom_erase;
static rp_rom_program_t g_rom_program;
static int              g_rom_ready;

/* The JEDEC RDID probe sits above the definitions of the copyout/XIP
 * helpers (they are RAMFUNC, section-placed below), so declare them here. */
static RAMFUNC void flash_init_boot2_copyout(void);
static RAMFUNC void flash_enable_xip_via_boot2(void);

static void *rp_rom_func(uint32_t code)
{
    rp_rom_lookup_t lookup =
        (rp_rom_lookup_t)(uintptr_t)(*(volatile uint16_t *)BOOTROM_LOOKUP_PTR);
    if (!lookup)
        return 0;
    /* The returned entry is a Thumb address; OR the bit in case the ROM
     * stored it aligned. */
    return (void *)((uintptr_t)lookup(code, RT_FLAG_FUNC_ARM_SEC) | 1u);
}

/* Tentative definition; the one with an initialiser is with the clock code
 * further down.  Declared here because the QMI helper must not reach into
 * flash for a frequency while XIP is off. */
static uint32_t g_clk_hz;


/* Issue JEDEC RDID (0x9F) through QMI direct mode and return the 3-byte
 * manufacturer/dev-id/capacity code as one 24-bit value, or 0 on failure.
 * The ROM cannot do this: FLASH_OP covers erase/program/read only, so the
 * chip select and the FIFO are driven by hand, exactly as pico-sdk's
 * flash_do_cmd_cs() does.
 *
 * This runs before the console has anything else to do but with XIP live,
 * so it uses the same ROM sequence the write path uses: copy the ROM's
 * XIP-setup routine out of bootram, connect the QSPI pins, drop XIP, talk
 * to the part in direct mode, restore XIP.  All RAM-resident; callers on
 * SMP take plat_flash_exclusive_begin() first so the other core is parked
 * out of flash.  The RDID itself must be clocked as 1 + 4 bytes (dummy
 * cycles clock the ID out): send the opcode followed by four zero bytes
 * and read back five, keeping the FIFO in lockstep.
 *
 * A JEDEC part answering 0xff/0x00 reads as a missing or floating bus and
 * returns 0 (mirrors the ESP32-C3 port's flash_detect_from_rom()). */
static RAMFUNC uint32_t rp2350_flash_detect_jedec(void)
{
    uint8_t tx[5] = {0x9fu, 0x00u, 0x00u, 0x00u, 0x00u};
    volatile uint8_t rx[5];
    uint32_t id, spins = 0;
    /* Fail fast.  This runs inside the XIP-off window, before the tick exists
     * to feed the watchdog, so a part that does not answer must not be waited
     * on for long.  C6/A-6 in docs/ARCHITECTURE-NEXT.md says a pre-tick wait
     * keeps an iteration cap and scales it with the clock; that rule assumed
     * an allowance the watchdog could afford.  Here it cannot: an RP2350 QMI
     * status read costs on the order of 100 cycles, so the old clock-scaled
     * bound (~1e6) turned one unanswered byte into ~0.4 s and the whole probe
     * into seconds - past the 2.1 s watchdog, which reset the board with no
     * output at all (#42, Pico 2 single-core ARM).  A part that answers at
     * all answers in microseconds, so the cap is sized for that instead of
     * for milliseconds it is not allowed to spend. */
    const uint32_t bound = 20000u;
    uint32_t csr0, clkdiv;
#if TOS_BOOT_TRACE
    uint32_t xip_pre;
#endif

    flash_init_boot2_copyout();
    g_rom_connect();
#if TOS_BOOT_TRACE
    /* A word from the image's vector table, read while XIP is still on; the
     * same address is read again after the boot2 restore. */
    xip_pre = *(const volatile uint32_t *)(XIP_BASE + 0x100u);
#endif
    g_rom_exit_xip();

    /* Keep CLKDIV: it is a field of this register, and the reset value the
     * ROM leaves is 6.  Writing EN|ASSERT alone divides by one, i.e. clocks
     * the flash at clk_sys, which no part on these boards answers at - see
     * the QMI_DIRECT_CSR_CLKDIV_BITS comment. */
    csr0 = mmio_read32(QMI_DIRECT_CSR);
    clkdiv = csr0 & QMI_DIRECT_CSR_CLKDIV_BITS;
    if (!clkdiv)
        clkdiv = QMI_DIRECT_CSR_CLKDIV_DEF;
#if TOS_BOOT_TRACE
    qmi_diag_puts(qmi_diag_csr0);
    qmi_diag_hex(csr0);
    qmi_diag_puts(qmi_diag_clkdiv);
    qmi_diag_hex((csr0 >> 22) & 0x3fu);
#endif

    /* Assert CS0n and enable direct mode. */
    mmio_write32(QMI_DIRECT_CSR,
                 clkdiv | QMI_DIRECT_CSR_EN | QMI_DIRECT_CSR_ASSERT_CS0N);

    /* Byte-by-byte FIFO exchange: each DIRECT_TX write clocks a byte in,
     * each DIRECT_RX read consumes the byte clocked by the previous write.
     * Poll for FIFO space and data; a missing part leaves RXEMPTY set, but
     * the exchange still drains because DIRECT_RX reads of an empty FIFO
     * return the last byte shifted (0xff on a floating bus). */
    for (uint32_t i = 0; i < 5u; i++) {
        /* Bounded, for the same reason as the RV32 port: a QMI block that
         * stops answering would otherwise be an invisible boot hang, with
         * the watchdog's 16.78 s reset as the only symptom. */
        for (spins = 0; spins < bound; spins++)
            if (!(mmio_read32(QMI_DIRECT_CSR) & QMI_DIRECT_CSR_TXFULL))
                break;
        if (spins == bound)
            break;
        mmio_write32(QMI_DIRECT_TX, tx[i]);
        for (spins = 0; spins < bound; spins++)
            if (!(mmio_read32(QMI_DIRECT_CSR) & QMI_DIRECT_CSR_RXEMPTY))
                break;
        if (spins == bound)
            break;
        rx[i] = (uint8_t)mmio_read32(QMI_DIRECT_RX);
    }

    /* Back to command-XIP mode: leave direct mode and put CLKDIV back the way
     * the ROM had it (writing 0 here would clear the divider as well). */
#if QMI_DIAG_OLD_EXIT
    mmio_write32(QMI_DIRECT_CSR, 0u);
#else
    mmio_write32(QMI_DIRECT_CSR, csr0);
#endif

    /* First byte is the RDID echo (0x9f); bytes 1..3 are the JEDEC ID.
     * A timed-out exchange leaves id at 0 = "unknown". */
    id = (spins == bound)
         ? 0u
         : (((uint32_t)rx[1] << 16u) | ((uint32_t)rx[2] << 8u) | rx[3]);
    if (id == 0u || id == 0xffffffu)
        id = 0u;
#if TOS_BOOT_TRACE
    qmi_diag_puts(qmi_diag_exit);
    qmi_diag_hex(mmio_read32(QMI_DIRECT_CSR));
    qmi_diag_puts(qmi_diag_id);
    qmi_diag_hex(id);
    qmi_diag_puts(qmi_diag_spins);
    qmi_diag_hex(spins);
#endif

    flash_enable_xip_via_boot2();
#if TOS_BOOT_TRACE
    qmi_diag_puts(qmi_diag_csr1);
    qmi_diag_hex(mmio_read32(QMI_DIRECT_CSR));
    qmi_diag_puts(qmi_diag_m0);
    qmi_diag_hex(mmio_read32(QMI_M0_TIMING));
    {
        uint32_t xip_post = *(const volatile uint32_t *)(XIP_BASE + 0x100u);

        qmi_diag_puts(qmi_diag_xip);
        qmi_diag_puts(xip_pre == xip_post ? qmi_diag_ok : qmi_diag_bad);
        qmi_diag_puts(qmi_diag_pre);
        qmi_diag_hex(xip_pre);
        qmi_diag_puts(qmi_diag_post);
        qmi_diag_hex(xip_post);
    }
    qmi_diag_puts(qmi_diag_nl);
#endif
    return id;
}

/* Decode a JEDEC capacity byte to bytes.  The third JEDEC byte encodes
 * log2(capacity in bytes) for the standard density table: 0x20=1 MiB,
 * 0x21=2 MiB, 0x22=4 MiB, 0x23=8 MiB, 0x24=16 MiB.  Only 1..16 MiB are
 * usable CS0 sizes here (the same bounds the ROM's CS0_SIZE decode and the
 * ESP32-C3 port's capacity-code check use); anything else - a non-JEDEC
 * part, a sub-MiB device, or a floating read - returns 0. */
static uint32_t rp2350_jedec_capacity(uint32_t jedec_id)
{
    uint32_t code = jedec_id & 0xffu;

    /* 20..24 decimal (0x14..0x18): 1<<20 = 1 MiB .. 1<<24 = 16 MiB.  The
     * bounds must be decimal - writing 0x20 here makes 1u << code a shift
     * of 32, which is undefined behaviour and lets the optimiser fold the
     * whole capacity decode to poison. */
    if (code < 20u || code > 24u)
        return 0;
    return 1u << code;
}

/* Read the CS0 flash capacity.  Two sources, in order of trust:
 *
 * 1. JEDEC RDID issued live through QMI direct mode (rp2350_flash_detect_
 *    jedec): this is what is actually on the board, whatever the board.conf
 *    says or the OTP was programmed with.
 * 2. The boot ROM's FLASH_DEVINFO word in bootram (data table 'F','D'),
 *    which only reflects the OTP FLASH_DEVINFO configuration - and with
 *    BOOT_FLAGS0_FLASH_DEVINFO_ENABLE unset it is the conservative default
 *    CS0_SIZE = 12 (16 MiB), an upper bound rather than a measurement.
 *
 * RDID is authoritative when it works; the ROM value is the fallback for a
 * part that does not answer (no flash, a bare-metal probe, a locked CS).
 * Mirrors the ESP32-C3 port's flash_detect_from_rom(), which reads the
 * JEDEC ID the ROM bootloader already fetched. */
static uint32_t rp2350_flash_detect(void)
{
    uint32_t jedec = rp2350_flash_detect_jedec();
    uint32_t bytes = rp2350_jedec_capacity(jedec);

    if (bytes)
        return bytes;

    /* Fallback: the ROM's recorded FLASH_DEVINFO.  Read it early: the ROM
     * only writes it at boot, but the copy could in principle be rewritten
     * later, and a stale read is worse than none. */
    {
        rp_rom_lookup_t lookup =
            (rp_rom_lookup_t)(uintptr_t)(*(volatile uint16_t *)BOOTROM_LOOKUP_PTR);
        uint32_t *ptr_literal;
        uint16_t devinfo;
        uint32_t size;

        if (!lookup)
            return 0;
        ptr_literal = (uint32_t *)lookup(ROM_DATA_FLASH_DEVINFO16_PTR,
                                         ROM_DATA_FLAG_DATA);
        if (!ptr_literal)
            return 0;
        /* The data table row holds a pointer to the bootram copy; dereference
         * twice: once for the 32-bit pointer literal, once for the 16-bit
         * FLASH_DEVINFO word. */
        devinfo = *(volatile uint16_t *)(uintptr_t)(*ptr_literal);
        size = (devinfo & FLASH_DEVINFO_CS0_SIZE_MASK)
               >> FLASH_DEVINFO_CS0_SIZE_LSB;

        /* Only 1/2/4/8/16 MiB are usable CS0 sizes (4096u << size, size
         * 0x8..0xc).  _NONE (0x0), the sub-MiB parts, and a missing/floating
         * table (0x0, 0xffff) all fail these bounds; ramdisk.c clamps the
         * detected value against the configured size, so a bogus 4 KiB would
         * otherwise shrink the filesystem to nothing. */
        if (size < 0x8u || size > 0xcu)
            return 0;
        return 4096u << size;
    }
}

static uint32_t g_flash_detected;

/* Bring-up convenience: a script on COM14 can send one Ctrl-D and have the
   ROM mount BOOTSEL again, instead of a human pressing BOOT + RST before
   every single reflash.  ROM_FUNC_REBOOT is 'R','B' = 0x4252; flags 0x102
   are REBOOT_TYPE_BOOTSEL (0x2) | NO_RETURN_ON_SUCCESS (0x100), the same
   call boot_probe.py uses to get back to the loader after a run. */
typedef void (*rp_rom_reboot_t)(uint32_t flags, uint32_t delay_ms,
                                uint32_t p0, uint32_t p1);
#define ROM_FUNC_REBOOT 0x4252u

/* Ask the ROM to reboot.  RT_FLAG_FUNC_ARM_SEC is the secure-state row of the
 * function table, which is where this kernel runs.  Returns only if the ROM
 * entry point could not be found or the call failed (NO_RETURN_ON_SUCCESS
 * makes success noreturn). */
static void rom_reboot(uint32_t flags)
{
    rp_rom_lookup_t lookup =
        (rp_rom_lookup_t)(uintptr_t)(*(volatile uint16_t *)BOOTROM_LOOKUP_PTR);
    rp_rom_reboot_t reboot;

    if (!lookup)
        return;
    reboot = (rp_rom_reboot_t)
        ((uintptr_t)lookup(ROM_FUNC_REBOOT, RT_FLAG_FUNC_ARM_SEC) | 1u);
    if (!reboot)
        return;
    reboot(flags, 10u, 0u, 0u);
}

static void rom_stop_other_core(void);   /* defined with the flash helpers */
static RAMFUNC void wdt_feed(void);       /* defined with the flash helpers */

/* Used by pll_sys_init() long before their definitions. */
static void early_puts(const char *s);
static void early_put_hex(uint32_t v);
static uint32_t measure_clk_src_khz(uint32_t src);

static void rom_enter_bootsel(void)
{
    rom_stop_other_core();
    rom_reboot(0x102u);           /* BOOTSEL | NO_RETURN_ON_SUCCESS */
    for (;;)
        ;
}

#define BOOTSEL_MAGIC_CHAR 0x04u      /* Ctrl-D, not a shell editing key */

/* Set the first time anything reads the console.  Until then plat_timer_tick()
 * is allowed to watch UART0 for Ctrl-D, so a kernel that hangs before the
 * shell starts can still be pushed back into the ROM bootloader. */
static volatile int g_console_claimed;

/* Installed as g_panic_poll: once the kernel has died, keep watching UART0
 * for Ctrl-D.  The RX FIFO works without interrupts, which is the same
 * reason plat_shutdown() can poll it, so a crashed image stays recoverable
 * from the host without anyone touching the BOOTSEL button. */
static void panic_poll_uart(void)
{
    if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE) &&
        (mmio_read32(RP_UART_DR) & 0xffu) == BOOTSEL_MAGIC_CHAR)
        rom_enter_bootsel();
}

/* ---- console: UART0 ----------------------------------------------------- */

#define UART_BAUD 115200u
/* ATK-DNRP2350AM (the board on the bench): the user LED is on GPIO3 and
 * is wired for sink current, i.e. LOW turns it ON.  GPIO25 - the Pico 2
 * convention - has no LED on this board at all. */
#define RP_LED_GPIO 3u

/* plat_board_early() prints its boot line before the USB stack exists, so
 * the console shunts the CDC calls until plat_console_init() has run. */
static int g_usb_ready;
/* Written once during console init (core 0, before the tick starts) and then
 * from the tick; read there too.  volatile keeps the compiler from folding
 * the two reads across the wait loop it sits in. */
static volatile int g_last_cfg = -1;

static int g_led_on;

static void led_set(int on)
{
    g_led_on = on ? 1 : 0;
    /* Active-low LED: clearing the output bit turns it ON. */
    mmio_write32(on ? SIO_GPIO_OUT_CLR : SIO_GPIO_OUT_SET, 1u << RP_LED_GPIO);
}

static void led_toggle(void)
{
    led_set(!g_led_on);
}

static void led_init(void)
{
    uint32_t pad = mmio_read32(RP_GPIO_PAD(RP_LED_GPIO));
    /* FUNCSEL first, then de-isolate the pad - the order pico-sdk uses.
     * Leaving ISO set keeps the pin electrically isolated even though every
     * register write succeeds, so the LED never lights. */
    mmio_write32(RP_GPIO_CTRL(RP_LED_GPIO), RP_GPIO_FUNC_SIO);
    mmio_write32(RP_GPIO_PAD(RP_LED_GPIO),
                 (pad | RP_PAD_IE) & ~(RP_PAD_OD | RP_PAD_ISO));
    mmio_write32(SIO_GPIO_OUT_SET, 1u << RP_LED_GPIO);  /* LED off (active low) */
    mmio_write32(SIO_GPIO_OE_SET, 1u << RP_LED_GPIO);
}

static void unreset(uint32_t bits)
{
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) & ~bits);
    for (uint32_t i = 0; i < plat_pre_tick_spins(1000000u); i++) {
        if ((mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & bits) == bits)
            return;
    }
}

/* clk_ref = XOSC (12 MHz) so FC0 has an honest reference.  clk_sys itself is
 * re-hosted on PLL_SYS by pll_sys_init() below; only clk_peri is pinned to
 * clk_sys here so the UART and the CPU share one rate. */
static void clocks_init(void)
{
    if (rp2350_xosc_start()) {
        mmio_write32(CLOCKS_BASE + CLK_REF_CTRL_OFFSET, CLK_REF_CTRL_SRC_XOSC);
        for (uint32_t i = 0; i < plat_pre_tick_spins(1000000u); i++) {
            if (mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & (1u << 2))
                break;
        }
    }
    /* clk_peri = clk_sys, undivided.  AUXSRC 0 is clk_sys on the RP2350. */
    mmio_write32(CLOCKS_BASE + CLK_PERI_DIV_OFFSET, CLK_DIV_1);
    mmio_write32(CLOCKS_BASE + CLK_PERI_CTRL_OFFSET,
                 CLK_PERI_CTRL_ENABLE |
                 (CLK_PERI_CTRL_AUXSRC_CLK_SYS << 5));
}

/* Move clk_sys onto PLL_SYS from SRAM.
 *
 * No QMI re-timing here, on purpose.  Measured on the Pico 2 (standalone
 * harness, 2026-11-18): the ROM leaves the part in EBh quad mode with
 * CLKDIV 3 / RXDELAY 50, and that timing still reads flash correctly at
 * 150 MHz - a 4 KiB checksum taken after the mux move is identical to the
 * one taken before it.  Writing our own M0_TIMING in the same step
 * (CLKDIV 4 / RXDELAY 1) is what breaks XIP: the sample point moves to
 * 3 ns after the clock edge instead of 167 ns, and the first fetch after
 * the switch reads garbage.  So the mux is the only thing that moves.
 *
 * This still runs from SRAM.  It keeps the mux write and the callers'
 * assumption that no flash fetch happens in between honest, and costs
 * nothing: .data.ramfunc is copied by _start before kmain. */
static RAMFUNC void clk_sys_to_pll_sys(void)
{
    uint32_t i;

    /* Same order as pico-sdk's clock_configure_internal(): before touching
     * AUXSRC, put the glitchless mux back on clk_ref (source 0) and wait for
     * it, otherwise changing the aux mux passes a glitch to the core.  The
     * ARM port never did this step; the RV32 port did it in pll_sys_init().
     * Then AUXSRC first, the glitchless source last, and wait for SELECTED. */
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET, 0u);
    for (i = 0; i < 100000u; i++) {
        if (mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET) & 1u)
            break;
    }

    mmio_write32(CLOCKS_BASE + CLK_SYS_DIV_OFFSET, CLK_DIV_1);
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET,
                 CLK_SYS_CTRL_AUX_PLL_SYS << 5);
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET,
                 CLK_SYS_CTRL_SRC_AUX |
                 (CLK_SYS_CTRL_AUX_PLL_SYS << 5));

    /* CLK_SYS_SELECTED must follow before anyone downstream assumes the
     * new rate; until it does the bus is still on the old source.  A fixed
     * spin count keeps this off plat_pre_tick_spins(), which is flash. */
    for (i = 0; i < 100000u; i++) {
        if (mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET) & (1u << 1))
            break;
    }
}

/* Park clk_sys on clk_ref (12 MHz XOSC/ROSC) before touching PLL_SYS.
 *
 * A UF2 download reboots into the new image without a power cycle, and the
 * bootrom can leave clk_sys running from a PLL.  Pulsing PLL_SYS's reset
 * while it is the live source stops the clock, so the chip never reaches
 * USB - the Pico 2 then only starts after the RUN button (issue #7: ATK
 * survived because its boot path happens to leave clk_sys on ROSC).  The
 * same step is a no-op on a cold boot, where clk_ref is already the source.
 * Runs from SRAM for the same reason as the PLL switch below. */
static RAMFUNC void clk_sys_to_clkref(void)
{
    uint32_t i;

    mmio_write32(CLOCKS_BASE + CLK_SYS_DIV_OFFSET, CLK_DIV_1);
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET, CLK_SYS_CTRL_SRC_CLKREF);
    for (i = 0; i < 100000u; i++) {
        if (mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET) & 1u)
            break;
    }
}

/* Bring PLL_SYS up at 150 MHz and switch clk_sys onto it.
 *
 * The ROM leaves clk_sys on the ~12 MHz ROSC/XOSC path; at that rate the
 * USB enumeration windows the TinyUSB SIE works inside are uncomfortably
 * tight and every busy-wait bound in the port is stretched to its limit.
 * The pico-sdk default for a 12 MHz crystal is used verbatim: REFDIV 1,
 * FBDIV 125 (12 MHz * 125 = 1500 MHz VCO, inside the 750-1600 window),
 * postdividers 5 and 2 = 150 MHz.
 *
 * Failure is benign: if the PLL never locks the function returns before
 * touching clk_sys and the boot continues on the ROM's clock.  Everything
 * downstream (UART divider, SysTick reload, SPI CPSR) is derived from the
 * value the frequency counter measures afterwards, never from this
 * constant, so both outcomes produce a correct board. */
static void pll_sys_init(void)
{
    const uint32_t refdiv  = 1u;
    const uint32_t fbdiv   = 125u;
    const uint32_t post1   = 5u;
    const uint32_t post2   = 2u;
    uint32_t i;
    uint32_t khz;

    /* The dividers below are the 12 MHz crystal's (125 x 12 MHz / 5 / 2).
     * If the oscillator did not come up - a cold start can leave clk_ref on
     * the ROSC, which is ~6.5 MHz and drifts with temperature - the same
     * dividers produce ~81 MHz, and raising clk_sys onto that breaks the
     * flash timing the ROM set up.  So the crystal has to be the selected
     * reference before the clock is raised at all; otherwise the boot
     * continues at the ROSC with divisors that were measured, not assumed. */
    if (!(mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & (1u << 2))) {
#if TOS_BOOT_TRACE
        early_puts("[B] crystal not selected, staying on clk_ref\r\n");
#endif
        return;
    }

    /* The live clock source is whatever the boot path left behind: ROSC on a
     * cold boot, but possibly a PLL after a UF2-triggered reboot.  Park
     * clk_sys on clk_ref first so the reset below cannot stop the clock. */
    clk_sys_to_clkref();

    /* Force a clean reset/unreset cycle: the ROM may have used this PLL for
     * its own XIP setup and left the dividers anywhere.  Same lesson as
     * PLL_USB in usb_cdc.c - a LOCK bit can be set on a PLL nobody
     * configured, so this boot configures it from scratch regardless. */
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) | RESET_PLL_SYS);
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) & ~RESET_PLL_SYS);
    for (i = 0; i < plat_pre_tick_spins(1000000u); i++) {
        if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & RESET_PLL_SYS)
            break;
    }

    /* Dividers are programmed while the VCO is powered down, exactly the
     * order pico-sdk's pll_init() uses. */
    PLL_SYS_CS_REG   = refdiv;
    PLL_SYS_FBDIV_REG = fbdiv;
    PLL_SYS_PRIM_REG = (post1 << 16) | (post2 << 12);
    PLL_SYS_PWR_REG &= ~(PLL_SYS_PWR_PD | PLL_SYS_PWR_VCOPD);

    for (i = 0; i < 2000000u; i++) {
        if (PLL_SYS_CS_REG & PLL_SYS_CS_LOCK)
            break;
    }
    if (!(PLL_SYS_CS_REG & PLL_SYS_CS_LOCK))
        return;                     /* no PLL: stay on the ROM's clk_sys   */

    PLL_SYS_PWR_REG &= ~PLL_SYS_PWR_POSTDIVPD;

    /* 150 MHz, but only if the PLL is really there.  On a cold start the
     * crystal is still drifting when the PLL reports LOCK, and moving
     * clk_sys onto it hangs the first boot: measured on the Pico 2, the
     * first boot after power-on died right here and the retry 1.2 s later
     * came up with the crystal warm (the ROM never ran - no UF2 volume
     * appeared).  So measure the PLL output and keep clk_sys on clk_ref
     * when it is not 150 MHz; the board then boots at 12 MHz instead of
     * not booting, and the next reset (crystal warm) gets the full rate. */
    khz = measure_clk_src_khz(FC0_SRC_PLL_SYS);
    if (khz < 149000u || khz > 151000u) {
#if TOS_BOOT_TRACE
        early_puts("[B] pll not used, measured 0x");
        early_put_hex(khz * 1000u);
        early_puts(" Hz\r\n");
#endif
        return;
    }

    /* Switch clk_sys onto the PLL output.  The divider is forced to 1.0
     * first, then SRC moves to the AUX mux (which now selects pll_sys).
     * Done from SRAM, where no flash fetch can happen between the mux write
     * and the switch being acknowledged - see clk_sys_to_pll_sys(). */
    clk_sys_to_pll_sys();
}

/* Measure clk_sys in kHz against clk_ref.  Returns 0 when the counter does
 * not settle; the caller then assumes the 12 MHz XOSC. */
/* Measure any FC0 source against the 12 MHz crystal.  A register read only
 * proves that a clock slice is *enabled*; the USB SIE needs clk_usb to be
 * exactly 48 MHz, and a PLL that reports LOCK can still be running at the
 * wrong divider.  Measuring is the only honest check, and an unimplemented
 * or stopped source returns 0 rather than a plausible-looking wrong number. */
static uint32_t measure_clk_src_khz(uint32_t src)
{
    uint32_t spins;
    const uint32_t bound = plat_pre_tick_spins(1000000u);
    /* FC0 counts against clk_ref, and clk_ref is not always the crystal:
     * clocks_init() leaves it on the ROSC when the 12 MHz crystal fails to
     * start.  Getting this reference wrong is invisible at the call site but
     * scales every measurement - and therefore the UART divisor and the
     * tick reload - by the ratio between the two, so read it from the mux
     * instead of assuming. */
    uint32_t ref_khz =
        (mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & (1u << 2))
            ? 12000u : 6500u;

    for (spins = 0; spins < bound; spins++) {
        if (!(mmio_read32(FC0_STATUS) & FC0_STATUS_RUNNING))
            break;
    }
    mmio_write32(FC0_REF_KHZ, ref_khz);
    mmio_write32(FC0_MIN_KHZ, 0u);
    mmio_write32(FC0_MAX_KHZ, 0xffffffffu);
    mmio_write32(FC0_INTERVAL, 10u);
    mmio_write32(FC0_SRC, src);                  /* writing starts it */

    for (spins = 0; spins < bound; spins++) {
        if (mmio_read32(FC0_STATUS) & FC0_STATUS_DONE)
            break;
    }
    if (spins == bound)
        return 0;
    return mmio_read32(FC0_RESULT) >> FC0_RESULT_KHZ_S;
}

static uint32_t measure_clk_khz(void)
{
    return measure_clk_src_khz(FC0_SRC_CLK_SYS);
}

static uint32_t g_clk_hz = ARM_SYSTICK_CLK;

#if TOS_DIAG
/* What the polling core sees when it looks for a park request.  If the flag
 * is set, the id differs from the owner and the loop still did not run, the
 * problem is inside the loop; if the flag reads as 0, the two cores are not
 * looking at the same variable. */
TOS_NOINIT volatile uint32_t g_park_poll_n;
TOS_NOINIT volatile uint32_t g_park_poll_flag;
TOS_NOINIT volatile uint32_t g_park_poll_id;
TOS_NOINIT volatile uint32_t g_park_poll_owner;

/* 0 idle, 1 initiator waits for the other core, 2 the other core is parked,
 * 3 inside the ROM call, 4 XIP back, 5 unparked. */
TOS_NOINIT volatile uint32_t g_park_phase;

/* Is the other core still alive while the initiator waits for it?  Its SysTick
 * count answers "still scheduling" versus "stopped" - the one thing the
 * initiator cannot tell from its own side. */
volatile uint32_t g_c1_ticks;
#endif

#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
/* Set by core 0 before it takes XIP away for a ROM flash call; the parking
 * block next to plat_block_erase is what acts on it. */
static struct rp_park_state park = RP_PARK_STATE_INIT;
/* The ack is a generation, not a flag.  With a boolean a stale "already
 * parked" left over from a timed-out handshake makes the next writer believe
 * the other core is off XIP when it is not, and the ROM then reads flash
 * under a core that is still fetching from it - a corrupt fetch, dead board,
 * no output. */
/* PARK-FIX: which core called rp_flash_park_other().  The tick and idle
 * checks must never park the initiator: it sits in a thread-context wait
 * loop that is the ONLY code which will ever clear the flag, and once its
 * SysTick catches the flag both cores spin in the RAM loop forever - the
 * first dd on atk-dnr2350am-smp froze the machine exactly that way. */
/* Set before the ROM bootloader takes the chip: the secondary core has to
 * stop for good.  The ROM does not reset it, so it would still be running
 * the old kernel when the new image starts - two kernels sharing one .bss,
 * and the board dies during the next flash write. */
static volatile int g_core1_stopped;
static void rp_flash_park_loop(void);
#endif

static void uart_init(void)
{
    uint32_t div;
    uint32_t pad;

    mmio_write32(RP_UART_CR, 0u);
    mmio_write32(RP_UART_ICR, 0x7ffu);           /* clear all interrupts */

    /* PL011: baud divisor = clk / (16 * baud).  The SDK's
     * (8*clk/baud + 1) trick rounds and separates integer/fraction. */
    div = (8u * g_clk_hz / UART_BAUD) + 1u;
    mmio_write32(RP_UART_IBRD, div >> 7);
    mmio_write32(RP_UART_FBRD, (div & 0x7fu) >> 1);
    mmio_write32(RP_UART_LCR_H, RP_UART_LCR_H_FEN | RP_UART_LCR_H_WLEN);

    /* GPIO0 = UART0 TX, GPIO1 = UART0 RX (FUNCSEL 2 on RP2350).  Each pad
     * must lose its reset-time ISO bit or the CH343 sees no edges at all. */
    mmio_write32(RP_GPIO_CTRL(0), RP_GPIO_FUNC_UART);
    pad = mmio_read32(RP_GPIO_PAD(0));
    mmio_write32(RP_GPIO_PAD(0),
                 (pad | RP_PAD_IE) & ~(RP_PAD_OD | RP_PAD_ISO));
    mmio_write32(RP_GPIO_CTRL(1), RP_GPIO_FUNC_UART);
    pad = mmio_read32(RP_GPIO_PAD(1));
    mmio_write32(RP_GPIO_PAD(1),
                 (pad | RP_PAD_IE) & ~(RP_PAD_OD | RP_PAD_ISO));

    mmio_write32(RP_UART_CR, RP_UART_CR_UARTEN |
                             RP_UART_CR_TXE | RP_UART_CR_RXE);
}

/* The console UART's live programming, for /proc/uart0.  Twelve words:
 * CR, FR, IBRD, FBRD, LCR_H, GPIO0_CTRL, PAD0, GPIO1_CTRL, PAD1,
 * CLK_PERI_CTRL, CLK_PERI_DIV, RESETS RESET_DONE.
 *
 * Read-only on purpose: this has to be safe to call from a *working* boot,
 * so it must not touch a register that could disturb the wire.  It is here
 * because the boot after a BOOTSEL flash loses UART0 TX while keeping RX,
 * and the CDC console is the only way to see what the UART is programmed
 * to do. */
void plat_uart_debug(uint32_t *out)
{
    out[0]  = mmio_read32(RP_UART_CR);
    out[1]  = mmio_read32(RP_UART_FR);
    out[2]  = mmio_read32(RP_UART_IBRD);
    out[3]  = mmio_read32(RP_UART_FBRD);
    out[4]  = mmio_read32(RP_UART_LCR_H);
    out[5]  = mmio_read32(RP_GPIO_CTRL(0));
    out[6]  = mmio_read32(RP_GPIO_PAD(0));
    out[7]  = mmio_read32(RP_GPIO_CTRL(1));
    out[8]  = mmio_read32(RP_GPIO_PAD(1));
    out[9]  = mmio_read32(CLOCKS_BASE + CLK_PERI_CTRL_OFFSET);
    out[10] = mmio_read32(CLOCKS_BASE + CLK_PERI_DIV_OFFSET);
    out[11] = mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET);
}

/* Transmit and watch the TX pin for the whole duration of each frame.
 *
 * SIO_GPIO_IN is the pad's view of every pin at once, so GPIO25 (the LED,
 * driven high by us) is the control: if GP25 reads high the readout works,
 * and what GP0 does is then a fact about GP0.  The sampling has to span the
 * whole 87 us frame, not just the moment after the write: a tight read loop
 * lands entirely inside the start bit, which is low by definition, and
 * "always low" would be read as "not driving" on a perfectly healthy pin.
 * (It was: 1533 low / 3 high, which was the start bit, not a dead pad.) */
int plat_uart_tx_probe(uint32_t *out)
{
    uint32_t lo = 0, hi = 0, trans = 0, prev = 2u, spins;
    uint32_t idle_lo = 0, idle_hi = 0, gp1_hi = 0, led_hi = 0;

    /* Baseline: what the pin reads with the transmitter idle.  A working
     * UART holds TX high between frames, so this is "must be 1". */
    for (int s = 0; s < 2048; s++) {
        uint32_t v = mmio_read32(SIO_GPIO_IN);

        if (v & 1u)
            idle_hi++;
        else
            idle_lo++;
    }

    for (int n = 0; n < 32; n++) {
        for (spins = 0; spins < 200000u; spins++)
            if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF))
                break;
        if (spins == 200000u)
            return 0;                   /* FIFO wedged: not this question */
        mmio_write32(RP_UART_DR, 0x55u);

        /* 2048 reads is ~27-80 us depending on the loop's cost: enough to
         * cross several bit times of the 87 us frame. */
        for (int s = 0; s < 2048; s++) {
            uint32_t v = mmio_read32(SIO_GPIO_IN);
            uint32_t pin = v & 1u;

            if (v & (1u << RP_LED_GPIO))
                led_hi++;
            if (v & 2u)
                gp1_hi++;
            if (pin)
                hi++;
            else
                lo++;
            if (prev != 2u && pin != prev)
                trans++;
            prev = pin;
        }
    }
    for (spins = 0; spins < 2000000u; spins++)
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_BUSY))
            break;
    out[0] = lo;
    out[1] = hi;
    out[2] = trans;
    out[3] = mmio_read32(RP_UART_FR);
    out[4] = idle_lo;
    out[5] = idle_hi;
    out[6] = gp1_hi;
    out[7] = led_hi;
    return 1;
}

/* Drive the TX pin from SIO for a moment and read it back.
 *
 * plat_uart_tx_probe() answers "does the UART move the pin".  When the
 * answer is no, this says which half is broken: the *same* pad driver, told
 * to output a 1 through a different peripheral, either pulls the pin high
 * (so the pad, the package ball and the trace are fine, and the problem is
 * whatever is on the other end of the wire or the UART's own output gate) or
 * cannot (so nothing at this pin can ever read high and no receiver will
 * ever see a frame, whatever the registers say).
 *
 * out[0]=samples high while driving 1, out[1]=low while driving 1,
 * out[2]=samples high while driving 0, out[3]=low while driving 0.
 */
int plat_gpio_drive_probe(uint32_t *out)
{
    uint32_t ctrl = mmio_read32(RP_GPIO_CTRL(0));
    uint32_t hi1 = 0, lo1 = 0, hi0 = 0, lo0 = 0;

    /* FUNCSEL 5 is SIO on every pin of both chips. */
    mmio_write32(RP_GPIO_CTRL(0), 5u);
    mmio_write32(SIO_GPIO_OE_SET, 1u);

    mmio_write32(SIO_GPIO_OUT_SET, 1u);
    out[4] = mmio_read32(RP_GPIO_CTRL(0));   /* must read 5 */
    out[5] = mmio_read32(SIO_BASE + 0x030u);  /* GPIO_OE, bit 0 */
    for (int s = 0; s < 2048; s++) {
        if (mmio_read32(SIO_GPIO_IN) & 1u)
            hi1++;
        else
            lo1++;
    }
    mmio_write32(SIO_GPIO_OUT_CLR, 1u);
    for (int s = 0; s < 2048; s++) {
        if (mmio_read32(SIO_GPIO_IN) & 1u)
            hi0++;
        else
            lo0++;
    }

    mmio_write32(SIO_GPIO_OE_CLR, 1u);
    mmio_write32(RP_GPIO_CTRL(0), ctrl);
    out[0] = hi1;
    out[1] = lo1;
    out[2] = hi0;
    out[3] = lo0;
    return 1;
}

#if TOS_DIAG
/* One line of liveness, printed straight to the PL011 (the console ring needs
 * the tick, and the tick is exactly what stops when the machine wedges).
 * Called from the kernel's spin-wait loop, so the last line before the freeze
 * says what each core was doing. */
static void diag_char(char c);        /* defined below */
static void diag_str(const char *s);
TOS_NOINIT static uint32_t g_diag_poll_n;

void plat_diag_poll(void)
{
    char b[12];
    int n;
    uint32_t v;

    /* Only report a wait that is actually stuck.  proc_lock is taken and
     * released hundreds of thousands of times a second by the idle console
     * path, so an unconditional periodic dump floods the wire with noise and
     * buries the one line that matters.  A healthy lock costs a handful of
     * retries; a deadlock runs to millions. */
    if (g_spin_count[0] < 200000u && g_spin_count[1] < 200000u)
        return;
    if ((++g_diag_poll_n & 0xfffffu) != 0u)
        return;

#define DIAG_PUT(s) do { const char *p_ = (s); while (*p_) diag_char(*p_++); } while (0)
#define DIAG_NUM(x) do { uint32_t v_ = (x); if (!v_) { diag_char('0'); } else {         n = 0; while (v_) { b[n++] = (char)('0' + v_ % 10u); v_ /= 10u; }         while (n) diag_char(b[--n]); } } while (0)
#define DIAG_HEX(x) do { uint32_t v_ = (x); diag_str("0x"); for (int i_ = 28; i_ >= 0; i_ -= 4) diag_char("0123456789ABCDEF"[(v_ >> i_) & 0xFu]); } while (0)

    DIAG_PUT("[H] c0="); DIAG_NUM(g_spin_wait[0]);
    DIAG_PUT(" c1="); DIAG_NUM(g_spin_wait[1]);
    DIAG_PUT(" lk0="); DIAG_HEX(g_spin_lock_addr[0]);
    DIAG_PUT(" lk1="); DIAG_HEX(g_spin_lock_addr[1]);
    DIAG_PUT(" sc0="); DIAG_HEX(g_spin_caller[0]);
    DIAG_PUT(" sc1="); DIAG_HEX(g_spin_caller[1]);
    DIAG_PUT(" n0="); DIAG_NUM(g_spin_count[0]);
    DIAG_PUT(" n1="); DIAG_NUM(g_spin_count[1]);
    DIAG_PUT(" isite="); DIAG_NUM(g_idle_site);
    DIAG_PUT(" us0="); DIAG_NUM(g_usb_site[0]);
    DIAG_PUT(" us1="); DIAG_NUM(g_usb_site[1]);
    DIAG_PUT(" mseq="); DIAG_NUM(g_mask_seq[0]);
    DIAG_PUT("/"); DIAG_NUM(g_mask_seq[1]);
    DIAG_PUT(" msite="); DIAG_NUM(g_mask_site[0]);
    DIAG_PUT("/"); DIAG_NUM(g_mask_site[1]);
    DIAG_PUT(" pc0="); DIAG_HEX(g_tick_pc[0]);
    DIAG_PUT(" pc1="); DIAG_HEX(g_tick_pc[1]);
    DIAG_PUT(" pk="); DIAG_NUM(g_park_phase);
    DIAG_PUT(" own="); DIAG_NUM(g_park_poll_owner);
    DIAG_PUT("\r\n");
    v = 0; (void)v;
#undef DIAG_PUT
#undef DIAG_NUM
}
#endif /* TOS_DIAG */

/* ---- watchdog ----------------------------------------------------------
 * A wedge in this kernel is silent: both cores sit in a spin loop, no tick
 * runs, and only a physical reset brings the board back - the ROM's UART
 * bootloader cannot even be entered because the CPU no longer reacts to
 * Ctrl-D.  The watchdog turns that into an automatic restart: the tick feeds
 * it while the system is alive, so a frozen tick stops feeding and the
 * board resets itself.
 *
 * RP2350 puts the watchdog at 0x400d8000.  The RP2040 address (0x40058000)
 * is PLL_USB on this chip, so the constant must not be copied over. */
#define WATCHDOG_BASE     0x400d8000u
#define WATCHDOG_CTRL_OFF 0x00u
#define WATCHDOG_LOAD_OFF 0x04u
#define WATCHDOG_CTRL_ENABLE  (1u << 30)
#define WATCHDOG_CTRL_TRIGGER (1u << 31)
/* ~2 s at the reset-default 1 us tick.  Small enough that a wedge anywhere -
 * including inside the ROM flash call, where the old code disarmed the
 * watchdog entirely and a hang became permanent - turns into a reset instead
 * of a dead board, large enough for one erase+program burst (~50-200 ms). */
#define WATCHDOG_RELOAD   0x001fffffu

static RAMFUNC void wdt_feed(void)
{
    mmio_write32(WATCHDOG_BASE + WATCHDOG_LOAD_OFF, WATCHDOG_RELOAD);
}

static RAMFUNC void wdt_enable(void)
{
    uint32_t ctrl = mmio_read32(WATCHDOG_BASE + WATCHDOG_CTRL_OFF);

    mmio_write32(WATCHDOG_BASE + WATCHDOG_CTRL_OFF, ctrl | WATCHDOG_CTRL_ENABLE);
    wdt_feed();
}

/* ---- boot-progress marks ------------------------------------------------ */

/* kmain() calls plat_boot_stage() between its stages.  Only the RV32 port had
 * an implementation, so on the ARM half a boot that stopped between [E] usb
 * and the banner left no trace at all - the [PREV] line only reports the
 * watchdog/tick state, and a stall in ramdisk_init() or screen_init() sets
 * none of it (#42).
 *
 * One SRAM store per stage and nothing else: the RV32 ring also samples UART
 * and clock registers and its own comment records that those reads perturb
 * the boot they measure.  A stage number cannot.  .noinit survives a reset,
 * so the next boot reports where the previous one stopped. */
#define BOOT_MARK_MAGIC 0x544f5342u     /* "TOSB" */
TOS_NOINIT static uint32_t g_boot_mark_magic;
TOS_NOINIT static uint32_t g_boot_mark_stage;

void plat_boot_stage(uint32_t n)
{
    g_boot_mark_magic = BOOT_MARK_MAGIC;
    g_boot_mark_stage = n;
    /* Each stage is proof of progress, so it also feeds the watchdog: the
     * dog is armed early (plat_board_early) but only the tick feeds it, and
     * the tick starts after plat_timer_init - a slow stage (the littlefs
     * mount on a 1 MiB image, say) would otherwise be reset with no output
     * at all.  One register write, no reads. */
    wdt_feed();
}

#if TOS_DIAG
/* Straight to the PL011, bypassing the console ring. */
static void diag_char(char c)
{
    uint32_t spins;

    for (spins = 0; spins < 20000u; spins++)
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF))
            break;
    mmio_write32(RP_UART_DR, (uint8_t)c);
}

static void diag_str(const char *s)
{
    while (*s)
        diag_char(*s++);
}

static void diag_u32(uint32_t v)
{
    char b[12];
    int n = 0;

    if (!v) {
        diag_char('0');
        return;
    }
    while (v) {
        b[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n)
        diag_char(b[--n]);
}

/* Bring-up marker for drivers: raw PL011, no console ring, no locks, so it can
 * be printed from inside a USB critical section without re-entering it. */
void plat_boot_mark(const char *s)
{
    diag_str(s);
}

void plat_boot_mark_u32(uint32_t v)
{
    diag_char('=');
    diag_u32(v);
}

void plat_boot_mark_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";

    diag_char('=');
    for (int i = 28; i >= 0; i -= 4)
        diag_char(hex[(v >> i) & 0xfu]);
}

/* ---- previous-boot evidence -------------------------------------------- */

/* A watchdog reset does not clear SRAM, and every diagnostic variable above
 * lives in .noinit, so the state that wedged the last boot is still on hand
 * when the next one starts.  Two constants guard against reporting random
 * power-on SRAM as evidence. */
#define DIAG_MAGIC0 0x544f5344u        /* "TOSD" */
#define DIAG_MAGIC1 0x31414744u        /* "1AGD" */

TOS_NOINIT static uint32_t g_diag_magic[2];

/* Live-stall detector state (see live_stall_check). */
TOS_NOINIT static uint32_t g_stall_ctr[2];
TOS_NOINIT static uint32_t g_stall_at[2];
TOS_NOINIT static uint32_t g_stall_said;

static void diag_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";

    for (int i = 28; i >= 0; i -= 4)
        diag_char(hex[(v >> i) & 0xfu]);
}

static void diag_kv(const char *k, uint32_t v)
{
    diag_str(k);
    diag_u32(v);
}

static void diag_kx(const char *k, uint32_t v)
{
    diag_str(k);
    diag_hex(v);
}

/* Print what the previous boot died doing, then start this one from a clean
 * slate.  Every field answers one question: maskN says which masked section
 * the core was inside when it stopped ticking (site id plus the caller's
 * return address - resolve it with addr2line), seqN frozen versus advancing
 * says wedged versus merely cycling, tickNpc is where the last tick caught
 * the other core, spinN is the lock address it was waiting on and n0/n1 how
 * many retries that wait had made. */
static void diag_prev_report(void)
{
    int have = (g_diag_magic[0] == DIAG_MAGIC0 &&
                g_diag_magic[1] == DIAG_MAGIC1);

    if (have) {
        diag_str("\r\n[PREV]");
        /* Where the previous boot stopped, in kmain() stage numbers: 1 timer,
         * 2 heap, 3 ramdisk, 4 devfs, 5 hwdev/screen, 6 process, 7 board
         * check, 8 banner, 9 cpu_init, 10 irq enable, 11 shell about to
         * start.  A number below 11 is the interesting case; 0 means the
         * mark itself is gone (fresh flash, or SRAM lost). */
        diag_kx(" ph=", g_boot_mark_magic == BOOT_MARK_MAGIC
                        ? g_boot_mark_stage : 0u);
        diag_kv(" m0s=", g_mask_site[0]);
        diag_kx(" m0lr=", g_mask_lr[0]);
        diag_kv(" m0n=", g_mask_seq[0]);
        diag_kv(" m1s=", g_mask_site[1]);
        diag_kx(" m1lr=", g_mask_lr[1]);
        diag_kv(" m1n=", g_mask_seq[1]);
        diag_kx(" pc0=", g_tick_pc[0]);
        diag_kx(" pc1=", g_tick_pc[1]);
        diag_kx(" sp0=", g_spin_lock_addr[0]);
        diag_kx(" sc0=", g_spin_caller[0]);
        diag_kv(" n0=", g_spin_count[0]);
        diag_kx(" sp1=", g_spin_lock_addr[1]);
        diag_kv(" n1=", g_spin_count[1]);
        diag_kv(" is=", g_idle_site);
        diag_kv(" pk=", g_park_phase);
        diag_kv(" pn=", g_park_poll_n);
        diag_kv(" pf=", g_park_poll_flag);
        diag_kv(" po=", g_park_poll_owner);
        diag_kv(" u0=", g_usb_site[0]);
        diag_kv(" u1=", g_usb_site[1]);
        diag_kv(" tc0=", g_tick_ctr[0]);
        diag_kv(" tc1=", g_tick_ctr[1]);
        diag_kv(" mi0=", g_mask_in[0]);
        diag_kv(" mi1=", g_mask_in[1]);
        diag_kv(" si0=", g_spin_in[0]);
        diag_kv(" si1=", g_spin_in[1]);
        diag_kv(" ki0=", g_park_in[0]);
        diag_kv(" ki1=", g_park_in[1]);
        diag_str("\r\n");
    }

    /* Fresh start for everything the report just consumed. */
    for (int i = 0; i < 2; i++) {
        g_mask_site[i] = 0;
        g_mask_lr[i] = 0;
        g_mask_seq[i] = 0;
        g_tick_pc[i] = 0;
        g_tick_lr[i] = 0;
        g_spin_lock_addr[i] = 0;
        g_spin_caller[i] = 0;
        g_spin_count[i] = 0;
        g_spin_wait[i] = 0;
        g_cpu_idle[i] = 0;
        g_usb_site[i] = 0;
    }
    g_idle_site = 0;
    g_park_phase = 0;
    g_park_poll_n = 0;
    g_park_poll_flag = 0;
    g_park_poll_id = 0;
    g_park_poll_owner = 0;
    g_diag_poll_n = 0;

    for (int i = 0; i < 2; i++) {
        g_tick_ctr[i] = 0;
        g_mask_in[i] = 0;
        g_spin_in[i] = 0;
        g_park_in[i] = 0;
        g_stall_ctr[i] = 0;
        g_stall_at[i] = 0;
    }
    g_stall_said = 0;

    /* Armed from here on: a watchdog reset during this boot leaves the
     * evidence below, and the next boot prints it. */
    g_diag_magic[0] = DIAG_MAGIC0;
    g_diag_magic[1] = DIAG_MAGIC1;
}

/* One core notices when the other stops taking ticks, and says what the other
 * was inside at that moment.
 *
 * This is the piece the earlier probes could not provide.  A snapshot taken
 * after the fact only says what a core did *last* - it cannot tell a core
 * wedged inside a critical section from one that left it safely, because
 * every counter looks the same from outside.  A per-core tick heartbeat plus
 * "is it inside a masked section / a spin / a park right now" answers the
 * question while the answer still exists, and the report comes from the
 * *healthy* core over the raw PL011, so it survives a console ring that
 * nothing is draining.
 *
 * The window is 1.2 s: below the watchdog's ~2.1 s so the report lands
 * before the reset, and far above any legitimate gap between ticks (1 ms,
 * with at most a few hundred ms lost to a flash write). */
static void live_stall_check(int id)
{
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    int o = id ^ 1;
    uint32_t now = g_tick_ctr[id];

    if (cpu_count <= 1 || g_tick_ctr[o] == 0u)
        return;                             /* other core not started yet */

    if (g_tick_ctr[o] != g_stall_ctr[o]) {
        g_stall_ctr[o] = g_tick_ctr[o];
        g_stall_at[o]  = now;
        return;
    }
    if (g_stall_said || (uint32_t)(now - g_stall_at[o]) < 1200u)
        return;
    g_stall_said = 1;

    diag_str("\r\n[STALL] live=");
    diag_u32((uint32_t)id);
    diag_kv(" dead=", (uint32_t)o);
    diag_kv(" t=", now);
    diag_kv(" mask=", g_mask_in[o]);
    diag_kv(" site=", g_mask_site[o]);
    diag_kx(" lr=", g_mask_lr[o]);
    diag_kv(" seq=", g_mask_seq[o]);
    diag_kv(" spin=", g_spin_in[o]);
    diag_kx(" lk=", g_spin_lock_addr[o]);
    diag_kx(" from=", g_spin_caller[o]);
    diag_kv(" n=", g_spin_count[o]);
    diag_kv(" park=", g_park_in[o]);
    /* SPINLOCK_ST (0x5C), not SPINLOCK[31]: reading a spinlock register
     * claims it, so sampling it here would wedge the system we are trying
     * to report on.  Bit 31 set means the kernel's SIO gate was taken at
     * this instant. */
    diag_kx(" greg=", mmio_read32(SIO_SPINLOCK_ST));
    diag_kv(" ph=", g_park_phase);
    diag_kv(" idle=", g_idle_site);
    diag_kv(" usb=", g_usb_site[o]);
    diag_kx(" pc=", g_tick_pc[o]);
    diag_str("\r\n");
#else
    (void)id;
#endif
}
#endif /* TOS_DIAG */

/* Park the secondary core and leave it there.  The ROM bootloader runs on
 * core 0 only and does not reset the chip when it is done, so without this
 * the secondary would still be running the old kernel when the new image
 * starts up - two kernels on one set of globals, which shows up as a board
 * that dies during its first flash write. */
static void rom_stop_other_core(void)
{
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    if (cpu_count <= 1)
        return;

    g_core1_stopped = 1;
    uint32_t myreq = rp_park_begin(&park, (int)plat_cpu_id());
    __asm__ volatile("sev" ::: "memory");

    for (uint32_t i = 0; i < plat_pre_tick_spins(20000000u); i++) {
        if (rp_park_acked(&park, myreq))
            break;
        wdt_feed();
    }
#else
    (void)0;
#endif
}

static void early_puts(const char *s)
{
    while (*s)
        plat_console_putc(*s++);
}

static void early_put_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 28; i >= 0; i -= 4)
        plat_console_putc(hex[(v >> i) & 0xfu]);
}

/* USBCTRL runs at priority 0x00 in drivers/usb/rp2350/tusb_shim.c.  A tick at
 * the same level cannot preempt it and vice versa, and the tick handler is
 * long enough (diagnostics, USB poll) to blow through the microseconds a
 * SET_ADDRESS status stage allows.  Keeping SysTick at 0x20 makes USB always
 * win that arbitration.  This hook is called by arm_exceptions_init() on
 * both cores, so the secondary sees the same value. */
uint32_t plat_arm_systick_priority(void)
{
    return 0x20u;
}

void plat_console_init(void)
{
    uart_init();
#if TOS_DIAG
    /* First thing on the wire: what the previous boot died doing, if it died
     * by watchdog.  Nothing has touched SRAM since the reset, so this is the
     * only moment the evidence exists. */
    diag_prev_report();
#endif
#if TOS_BOOT_TRACE
    early_puts("[P1] uart ready, usb init ...\r\n");
#endif
    usb_cdc_console_init(g_clk_hz);
#if TOS_BOOT_TRACE
    early_puts("[P2] usb init returned\r\n");
#endif

    /* SysTick priority is set by plat_arm_systick_priority() below, from
     * inside arm_exceptions_init().  Writing SHPR3 here would be undone a
     * moment later by plat_init(), and on the secondary core the same
     * hand-written value would never be applied at all. */

    /* Only let the console shunt characters into the CDC ring when the
       controller actually came up.  With the PLL handshake failed (stage 2)
       there is no endpoint to drain it, so every boot character would just
       fill a 512-byte ring and then be dropped one by one - and every tick
       would poll a device that is still held in reset. */
    g_usb_ready = usb_cdc_hw_ready() ? 1 : 0;

    /* Say hello again on the wire that may have enumerated a moment ago.
     * The UART already saw this line in plat_board_early(); the USB side
     * could not, because the host had not finished SET_CONFIGURATION yet. */
    /* Boot diagnostic.  hw=0 means usb_cdc_init() bailed on the reset/
     * PLL handshake; hw=1/cfg=0 means the PHY is up but the host never
     * finished enumeration.  The measured-clock line sits next to it: the
     * SIE bit-engine needs clk_usb to be exactly 48 MHz to recover bits
     * off the wire, and a PLL that reports LOCK at the wrong divider still
     * detects SE0 but silently drops every packet - printing what FC0
     * actually measured separates those cases in one glance. */
#if TOS_BOOT_TRACE
    early_puts("[E] usb hw=");
    early_puts(usb_cdc_hw_ready() ? "1" : "0");
    early_puts(" cfg=");
    early_puts(usb_cdc_configured() ? "1" : "0");
    early_puts(" stage=");
    plat_console_putc((char)('0' + usb_cdc_init_stage()));
    early_puts(" clk_sys_khz=");
    early_put_hex(measure_clk_src_khz(FC0_SRC_CLK_SYS));
    early_puts(" clk_usb_khz=");
    early_put_hex(measure_clk_src_khz(0x0bu));    /* FC0_SRC = CLK_USB */
    early_puts("\r\n");
    /* The state is on the wire now; the tick's change tracer must not repeat
     * it one line later.  It only speaks when the host later re-configures
     * (cable pulled, replugged). */
    g_last_cfg = usb_cdc_configured();
#endif

    if (usb_cdc_configured()) {
#if TOS_BOOT_TRACE
        early_puts("\r\nTinyOS/RP2350  usb-cdc=up  clk=0x");
        early_put_hex(g_clk_hz);
        early_puts(" Hz\r\n");
#endif
        led_set(0);                 /* enumerated: normal boot          */
    }
    /* USB did not come up: leave the LED lit as a visible "look at UART0"
     * hint rather than pretending the boot succeeded. */

    /* Last step: let the controller interrupt fire.  dcd_init() already
     * registered its handler and armed INTE; this only opens the NVIC gate,
     * so a controller event during the rest of bring-up could not have run
     * before the console was ready. */
    usb_cdc_irq_enable();
}

void plat_console_putc(char c)
{
    uint32_t spins;

    /* The PL011 FIFO must never be able to stall the kernel forever (a
     * board with no adapter still has to reach the USB console). */
    for (spins = 0; spins < 100000u; spins++) {
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF))
            break;
    }
    /* Write even if the bounded wait expired: a gated clk_peri can make FR
     * read as "FIFO full" forever, and silently dropping every byte turns
     * a diagnosable boot into a black box.  Worst case is a dropped marker
     * character, not a lost boot log. */
    mmio_write32(RP_UART_DR, (uint8_t)c);
    if (g_usb_ready)
        usb_cdc_putc(c);
}

void plat_console_flush(void)
{
    uint32_t spins;

    for (spins = 0; spins < 100000u; spins++) {
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_BUSY))
            break;
    }
    if (g_usb_ready)
        usb_cdc_flush();
}

int plat_console_tx_room(void)
{
    /* A 16-deep FIFO.  Reporting the whole FIFO when not full is enough
     * for the echo path to avoid a blocking write inside the tick.  The CDC
     * ring is larger, so the UART is the limiting side whenever both are
     * connected; usb_cdc_putc() drops rather than block if it ever fills. */
    return (mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF) ? 0 : 16;
}

int plat_console_can_take(int need)
{
    /* The CDC mirror drops whatever does not fit its ring, so the console
     * write() path has to come back short while it is full.  Only once the
     * host has configured the port: before that putc() drops anyway and the
     * ring is not a sink whose room means anything.  console_write_some()
     * bounds how long the callers may keep waiting on a host that never
     * drains, so this stays a pure "is there room right now" answer. */
    if (g_usb_ready && usb_cdc_configured() && usb_cdc_tx_room() < need)
        return 0;
    return 1;
}

/* TinyUSB's state machine needs real thread context.  tty_poll() is also
 * called with interrupts masked, from the tick and from the block device
 * while a task waits for flash; letting it call usb_cdc_poll() was the RV32
 * port's boot hang (see the matching comment in board/rp2350rv-generic).  Run the
 * state machine from the scheduler's idle loop instead and let the console
 * read path touch only the completed-byte queue.
 *
 * This is also the board's deferred-work hook (g_idle_poll), so arch/arm
 * runs it from PendSV once per tick as well - the idle loop is not reached
 * while a task is runnable, and that is exactly when a long console write
 * has to keep draining (issue #46). */
extern void (*g_idle_poll)(void);

static void usb_idle_poll(void)
{
    if (plat_cpu_id() == 0 && g_usb_ready)
        usb_cdc_poll();
}

int plat_console_getchar(void)
{
    int c;

    g_console_claimed = 1;

    /* USB belongs to core 0.  Core 1 therefore reads the UART only; USB
     * bytes still reach a reader on either core, because core 0's idle
     * poll drains the device into the tty line buffer.  The state machine
     * itself is not run here: this function is also reached from
     * tty_poll() with interrupts masked. */
    if (plat_cpu_id() == 0 && g_usb_ready) {
        c = usb_cdc_getchar();
        if (c >= 0) {
            /* Same escape hatch over CDC as over UART0: the ROM reboot does
             * not care which console delivered the byte.  Both consoles are
             * checked because a host with only the USB cable attached has no
             * other way back into BOOTSEL. */
            if (c == BOOTSEL_MAGIC_CHAR)
                rom_enter_bootsel();
            return c;
        }
    }
    if (mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE)
        return -1;
    c = (int)(mmio_read32(RP_UART_DR) & 0xffu);
    /* Handles the whole bring-up loop from a script: one byte on COM14 and
       the board is back in BOOTSEL, no button press.  This is polled from
       the tick, so it works even while the shell is blocked in read(). */
    if (c == BOOTSEL_MAGIC_CHAR)
        rom_enter_bootsel();
    return c;
}

/* ---- SysTick ----------------------------------------------------------- */

static volatile uint64_t g_ticks;
static uint32_t g_reload;

void plat_timer_init(uint32_t hz)
{
    g_reload = g_clk_hz / hz;
    if (g_reload == 0)
        g_reload = 1;
    g_ticks = 0;

    mmio_write32(ARM_SYSTICK_LOAD, g_reload - 1u);
    mmio_write32(ARM_SYSTICK_VAL, 0);
    /* ENABLE | CLKSOURCE (processor clock).  TICKINT stays masked until
     * plat_irq_enable() calls plat_timer_irq_unmask(). */
    mmio_write32(ARM_SYSTICK_CTRL, 5u);
}

uint64_t plat_ticks(void)
{
    return g_ticks;
}

uint32_t plat_tick_hz(void)
{
    return g_clk_hz / g_reload;
}

uint32_t plat_uptime_ms(void)
{
    return (uint32_t)((g_ticks * 1000u) / (uint64_t)plat_tick_hz());
}

uint64_t plat_now_ns(void)
{
    static uint64_t last_ns;
    uint32_t load = g_reload;
    uint32_t val  = mmio_read32(ARM_SYSTICK_VAL) & 0xFFFFFFu;
    uint32_t into = (load > val) ? (load - val) : 0u;
    uint64_t cycles = g_ticks * (uint64_t)load + into;
    uint64_t ns = (cycles / (uint64_t)g_clk_hz) * 1000000000ull
                + ((cycles % (uint64_t)g_clk_hz) * 1000000000ull)
                  / (uint64_t)g_clk_hz;

    if (ns < last_ns)
        ns = last_ns;
    last_ns = ns;
    return ns;
}

void plat_timer_tick(void)
{
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    /* Core 0 is about to turn XIP off for a ROM flash call: park here, in
     * RAM, before the next instruction fetch can touch flash.  Never park
     * the core that *initiated* the flash call - it is inside the thread
     * wait that is the only code which will ever clear the flag. */
    if (rp_park_should_park(&park, (int)plat_cpu_id()))
        rp_flash_park_loop();
#endif
    /* Only the boot core counts time.  Every core needs its own tick - it is
     * what preempts whatever that core is running - but the counter they
     * share is global: two cores incrementing it would run the clock at 2x
     * and expire every sleep deadline twice as fast.  Core 0's tick keeps
     * running even when it is idle, so this stays a true wall clock.
     * (Same rule as board/qemu-virt.) */
    int id = plat_cpu_id();

#if TOS_DIAG
    /* Before anything else in the tick: if the other core has stopped
     * ticking, say so now, while what killed it is still in memory. */
    live_stall_check(id);
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    if (id == 1)
        g_c1_ticks++;
#endif
#endif

    if (id == 0) {
        g_ticks++;              /* SysTick reloads itself */
        /* Feed the watchdog often enough that a busy tick cannot starve it,
         * but rarely enough not to sit on the bus: 50 ms is far below the
         * count-down and well above the worst-case gap between ticks. */
        if ((g_ticks % 50u) == 0u)
            wdt_feed();
    }

    if (id != 0)
        return;                 /* the rest of this is core-0 only: USB,
                                 * the LED heartbeat and the BOOTSEL poll all
                                 * belong to the console core */

    /* NO USB work in the tick.  plat_timer_tick() is the SysTick handler, so
     * anything it calls runs on the exception stack with interrupts masked;
     * TinyUSB's task needs the caller's stack and can take a while.  The
     * controller interrupt still handles the SIE events themselves; the task
     * loop runs from plat_cpu_idle(), from the console read path, and from
     * the deferred-work boundary arch/arm pends on every tick that
     * interrupted user code (PendSV, see arm_handle_pendsv).  The last one is
     * what keeps the console alive when some task is runnable all the time and
     * the idle loop is never reached - issue #46. */

    /* Bring-up trace, one line per state change.  The host may take many
     * seconds to start enumerating and the tick is the only thing that
     * notices, so the log says when the device actually became configured
     * rather than dumping the SIE state every 100 ms.  cfg=1 means
     * SET_CONFIGURATION has been processed and the CDC port exists on the
     * host side; anything else is still in flight. */
#if TOS_BOOT_TRACE
    if (!tty_line_taken() && g_usb_ready && g_ticks <= 120000u &&
        (g_ticks % 500u) == 0u) {
        int cfg = usb_cdc_configured();
        if (cfg != g_last_cfg) {
            g_last_cfg = cfg;
            /* Start on a fresh line and stop as soon as the shell has taken
             * a command: glued to "tos> " this trace reads as garbled
             * output, and it has no value once someone is typing. */
            early_puts("\r\n[U] cfg=");
            plat_console_putc(cfg ? '1' : '0');
            early_puts("\r\n");
        }
    }
#endif

    /* A 1 Hz heartbeat (tick is 1000 Hz, so one toggle per second gives a
     * 0.5 Hz square wave).  This is the ONLY boot signal that needs no
     * wiring: solid LED = the kernel died before the timer started, a
     * blinking LED = the kernel is alive and the shell should be up, no
     * matter whether USB CDC or the UART adapter ever enumerates. */
    if ((g_ticks % 1000u) == 0u && !plat_led_owned())
        led_toggle();

    /* Escape hatch for a kernel that hangs before the shell exists.  The
     * shell is normally the only reader of UART0, so a hang before it starts
     * leaves Ctrl-D unread and the board only recoverable with the physical
     * BOOTSEL button.  While nothing has claimed the console yet, the tick
     * peeks at the RX FIFO itself; the moment plat_console_getchar() is first
     * called the claim is recorded and this stops for good, so no keystroke
     * is ever stolen from a running shell. */
    if (!g_console_claimed && (g_ticks % 10u) == 0u &&
        !(mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE)) {
        if ((mmio_read32(RP_UART_DR) & 0xffu) == BOOTSEL_MAGIC_CHAR)
            rom_enter_bootsel();
    }
}

void plat_timer_irq_unmask(void)
{
    uint32_t ctrl = mmio_read32(ARM_SYSTICK_CTRL);
    if (!(ctrl & 2u))
        mmio_write32(ARM_SYSTICK_CTRL, ctrl | 2u);
}

/* A secondary core arms its own SysTick: LOAD/VAL/CTRL are per core, and
 * nothing core 0 wrote applies to us.  TICKINT goes in here (bit 1) rather
 * than through plat_timer_irq_unmask(), which would be happy to see core 0's
 * earlier unmask and skip us. */
void plat_timer_start_cpu(void)
{
    if (g_reload == 0)
        return;
    mmio_write32(ARM_SYSTICK_LOAD, g_reload - 1u);
    mmio_write32(ARM_SYSTICK_VAL, 0);
    mmio_write32(ARM_SYSTICK_CTRL, 7u);     /* ENABLE | TICKINT | CLKSOURCE */
}

/* ---- bringing core 1 up ------------------------------------------------ */

/* Core 1 does not execute our reset vector.  It sits in the boot ROM's
 * wait-for-FIFO loop until core 0 pushes a launch sequence at it, and the
 * ROM then loads VTOR, SP and PC from the three words that follow the
 * handshake.  This is the same protocol pico-sdk's
 * multicore_launch_core1_raw() uses; each word is echoed back, which is how
 * both sides know the other one is alive:
 *
 *     0  0  1  <vector table>  <stack pointer>  <entry>
 *
 * The two leading zeros are not padding: they make the ROM drain whatever
 * stale words a previous run left in its mailbox, and the SDK restarts the
 * whole sequence if an echo comes back wrong.  A stray "0,0,1" with no
 * follow-through is harmless because the ROM goes back to waiting.
 *
 * The stack is core 1's slice of the kernel stack pool and the entry is
 * arm_core1_entry() in arch/arm/start.S, which re-asserts MSP and VTOR and
 * then joins the scheduler. */
extern void arm_core1_entry(void);
extern char __vector_table[];

#define SIO_FIFO_ST (SIO_BASE + 0x050u)
#define SIO_FIFO_WR (SIO_BASE + 0x054u)
#define SIO_FIFO_RD (SIO_BASE + 0x058u)
#define SIO_FIFO_VLD (1u << 0)      /* read: something to pop   */
#define SIO_FIFO_RDY (1u << 1)      /* write: room for another  */

static void sio_fifo_push(uint32_t v)
{
    for (uint32_t spins = 0; spins < 100000u; spins++) {
        if (mmio_read32(SIO_FIFO_ST) & SIO_FIFO_RDY)
            break;
    }
    mmio_write32(SIO_FIFO_WR, v);
    __asm__ volatile("sev" ::: "memory");   /* the other core may be in WFE */
}

static uint32_t sio_fifo_pop(void)
{
    for (uint32_t spins = 0; spins < 100000u; spins++) {
        if (mmio_read32(SIO_FIFO_ST) & SIO_FIFO_VLD)
            return mmio_read32(SIO_FIFO_RD);
        __asm__ volatile("wfe" ::: "memory");
    }
    return 0xFFFFFFFFu;         /* nothing came back: the launch failed */
}

static void sio_fifo_drain(void)
{
    while (mmio_read32(SIO_FIFO_ST) & SIO_FIFO_VLD)
        (void)mmio_read32(SIO_FIFO_RD);
}

int plat_start_cpu(int id)
{
    const uint32_t lead[3] = { 0u, 0u, 1u };
    uint32_t tail[3];
    uint32_t seq = 0;
    uint32_t cmd;
    int restarts = 0;

    if (id != 1)
        return 0;               /* this chip has exactly two cores */

    tail[0] = (uint32_t)(uintptr_t)__vector_table;
    tail[1] = CPU_KSTACK_TOP(1u);
    tail[2] = (uint32_t)(uintptr_t)arm_core1_entry | 1u;  /* Thumb bit */

    for (;;) {
        cmd = (seq < 3) ? lead[seq] : tail[seq - 3];

        if (cmd == 0) {
            /* Drain first: a stale word from an earlier attempt would be
             * echoed as the "response" to this one. */
            sio_fifo_drain();
            __asm__ volatile("sev" ::: "memory");
        }
        sio_fifo_push(cmd);

        if (sio_fifo_pop() != cmd) {
            /* Out of step.  The SDK restarts the whole sequence; only a core
             * that never answers at all is a failure. */
            seq = 0;
            if (++restarts > 4)
                return 0;
            continue;
        }
        if (++seq == 6)
            return 1;           /* core 1 is running arm_core1_entry() */
    }
}

/* ---- block device: NOR flash, or a RAM disk when the board has none ----- */

/* The UF2 packer puts littlefs at flash offset 0x80000, which is
 * PLAT_LFS_LBA * 512.  Erase is 4 KiB aligned (8 littlefs sectors).
 *
 * A board with `fs_backing = embedded` has no partition: the build links the
 * same littlefs image into .data.ramdisk, and block 0 is block 0 of that
 * buffer (see writeRamdiskBlob in internal/build). */

#define RP_FLASH_SECTOR      4096u
#define RP_SECTOR_ERASE_CMD  0x20u

extern uint8_t __ramdisk_start[];
extern uint8_t __ramdisk_end[];

int plat_block_read(uint32_t lba, uint32_t count, void *buf)
{
#if TOS_HAS_FLASH
    const void *src = (const void *)(XIP_BASE + lba * 512u);
    memcpy(buf, src, count * 512u);
    return 0;
#else
    uint32_t off = lba * 512u;
    uint32_t len = count * 512u;
    if (off + len < off || off + len > (uint32_t)(__ramdisk_end - __ramdisk_start))
        return -1;
    memcpy(buf, __ramdisk_start + off, len);
    return 0;
#endif
}

/* Restore XIP the way pico-sdk's flash_range_erase/program does.
 *
 * The boot ROM leaves a 256-byte XIP-setup routine in bootram: the one its
 * flash scan used, i.e. the fast read mode and read delay it found for this
 * chip.  The SDK copies it to RAM and calls it after flash_flush_cache();
 * calling flash_enter_cmd_xip() instead installs the ROM's own basic mode
 * (03h serial read, CLKDIV=12), whose read delay is counted in clk_sys cycles
 * and is therefore 12.5x too short once clk_sys is at 150 MHz.  The first
 * instruction fetched after the write is then sampled before the flash has
 * driven it, and the core dies on the spot.  At 12 MHz the same mode still
 * works, which is what made the failure look clock-dependent.
 *
 * The +1 is the Thumb bit; pico-sdk notes it is harmless on RISC-V because
 * JALR clears bit 0 of the target. */
#define BOOTRAM_BASE     0x400e0000u
#define BOOT2_SIZE_WORDS 64u

static uint32_t boot2_copyout[BOOT2_SIZE_WORDS];
static int      boot2_copyout_valid;

static RAMFUNC void flash_init_boot2_copyout(void)
{
    const volatile uint32_t *src = (const volatile uint32_t *)BOOTRAM_BASE;

    if (boot2_copyout_valid)
        return;
    for (uint32_t i = 0; i < BOOT2_SIZE_WORDS; i++)
        boot2_copyout[i] = src[i];
    boot2_copyout_valid = 1;
}

static RAMFUNC void flash_enable_xip_via_boot2(void)
{
    ((void (*)(void))(uintptr_t)((uintptr_t)boot2_copyout + 1u))();
}

#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
/* ---- parking the other core out of XIP ---------------------------------- */

/* While the ROM has QMI in direct mode, any XIP access from the other core is
 * a bus fault, so the secondary core has to be off flash before the first
 * byte of the sequence.  Same scheme as the RP2040 port: a flag, a RAM-resident
 * spin loop, a check in the tick (which preempts whatever core 1 is running)
 * and a check in idle (a core already inside its tick cannot be preempted
 * again, so the tick check alone would never see the flag). */
/* RAM-resident: this runs while the owner has XIP turned off.  The shared
 * loop re-publishes the ack for every generation (a publish-once ack is the
 * "dd: write error at block 0 (-1)" defect) and feeds the watchdog, because
 * this core is parked with interrupts masked and the tick cannot.  After the
 * loop, g_core1_stopped keeps the core here for good before a ROM BOOTSEL. */
static RAMFUNC void rp_flash_park_loop(void)
{
    RP_PARK_PARKED_LOOP(&park, plat_pre_tick_spins(200000000u));
}


void plat_flash_park_poll(void)
{
#if TOS_DIAG
    g_park_poll_n++;
    g_park_poll_flag = (uint32_t)park.flag;
    g_park_poll_id = (uint32_t)plat_cpu_id();
    g_park_poll_owner = (uint32_t)park.owner;
#endif

    if (rp_park_should_park(&park, (int)plat_cpu_id())) {
        /* Mask interrupts BEFORE publishing the acknowledgement.  The park
         * loop starts by telling the writer "I am in RAM now"; if anything
         * can preempt this core after that point, the pre-emption returns to
         * flash - the USB interrupt is priority 0 and its handler lives in
         * XIP - and the writer has already put QMI into serial command mode.
         * The fetch then bus-faults inside an exception whose own handler is
         * also in flash: silent dead core, no ticks, no output, watchdog
         * reset two seconds later.  That is the atk-dnr2350am-smp dd wedge.
         *
         * The tick path already ran masked, which is why only the idle and
         * spin-wait callers ever hit this. */
        uint32_t pm = plat_irq_save();

        rp_flash_park_loop();
        plat_irq_restore(pm);
    }
}

/* Mask interrupts for the whole handshake: with them on, the tick can land in
 * this wait loop and reschedule the caller onto the other core, which then is
 * no longer the one that will unpark - both cores park and the flag is never
 * cleared (the intermittent atk-dnr2350am-smp dd freeze).  The shared sequence keeps
 * the mask on success; on timeout it restores and returns -1. */
static int rp_flash_park_other(void)
{
    RP_PARK_OWNER_TAKE(&park, plat_pre_tick_spins(20000000u));
}

static void rp_flash_unpark_other(void)
{
    RP_PARK_RELEASE_AND_WAKE(&park);
}
#endif

int plat_flash_exclusive_begin(void)
{
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    return rp_flash_park_other();
#else
    return 0;
#endif
}

void plat_flash_exclusive_end(void)
{
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    rp_flash_unpark_other();
#endif
}

RAMFUNC int plat_block_erase(uint32_t lba, uint32_t count)
{
#if !TOS_HAS_FLASH
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t off = (lba + i) * 512u;
        if (off + 512u > (uint32_t)(__ramdisk_end - __ramdisk_start))
            return -1;
        memset(__ramdisk_start + off, 0xFF, 512);
    }
    return 0;
#else
    uint32_t addr, end, was_enabled;

    if (!g_rom_ready)
        return -1;

    addr = (lba * 512u) & ~(RP_FLASH_SECTOR - 1u);
    end  = ((lba + count) * 512u + RP_FLASH_SECTOR - 1u)
           & ~(RP_FLASH_SECTOR - 1u);

    __asm__ volatile("mrs %0, primask" : "=r"(was_enabled));
    if (plat_flash_exclusive_begin() != 0)
        return -1;
    __asm__ volatile("cpsid i" ::: "memory");
    /* Keep the watchdog armed: it used to be disabled around the ROM call,
     * so a wedge inside the sequence was silent and permanent.  Feed instead -
     * one erase burst is ~50 ms, well inside the reload. */
    wdt_feed();
#if TOS_DIAG
    g_park_phase = 3;                       /* inside the ROM call */
#endif
    flash_init_boot2_copyout();
    g_rom_connect();
    g_rom_exit_xip();
    g_rom_erase(addr, end - addr, RP_FLASH_SECTOR, RP_SECTOR_ERASE_CMD);
    g_rom_flush();
    wdt_feed();
#if TOS_DIAG
    g_park_phase = 4;                       /* XIP is back */
#endif
    flash_enable_xip_via_boot2();
    plat_flash_exclusive_end();
#if TOS_DIAG
    g_park_phase = 5;                       /* unparked */
#endif
    if (!(was_enabled & 1u))
        __asm__ volatile("cpsie i" ::: "memory");
    return 0;
#endif
}

RAMFUNC int plat_block_write(uint32_t lba, uint32_t count, const void *buf)
{
#if !TOS_HAS_FLASH
    uint32_t off = lba * 512u;
    uint32_t len = count * 512u;
    if (off + len < off || off + len > (uint32_t)(__ramdisk_end - __ramdisk_start))
        return -1;
    memcpy(__ramdisk_start + off, buf, len);
    return 0;
#else
    uint32_t addr, was_enabled;

    if (!g_rom_ready)
        return -1;

    /* littlefs always writes whole 512-byte sectors: 256-byte flash page
     * alignment and the count multiple are satisfied.  The bounce buffer in
     * ramdisk.c is in SRAM, so the ROM never reads its source from XIP. */
    addr = lba * 512u;

    __asm__ volatile("mrs %0, primask" : "=r"(was_enabled));
    if (plat_flash_exclusive_begin() != 0)
        return -1;
    __asm__ volatile("cpsid i" ::: "memory");
    wdt_feed();                            /* stay armed; see plat_block_erase */
#if TOS_DIAG
    g_park_phase = 3;                       /* inside the ROM call */
#endif
    flash_init_boot2_copyout();
    g_rom_connect();
    g_rom_exit_xip();
    g_rom_program(addr, (const uint8_t *)buf, count * 512u);
    g_rom_flush();
    wdt_feed();
#if TOS_DIAG
    g_park_phase = 4;                       /* XIP is back */
#endif
    flash_enable_xip_via_boot2();
    plat_flash_exclusive_end();
#if TOS_DIAG
    g_park_phase = 5;                       /* unparked */
#endif
    if (!(was_enabled & 1u))
        __asm__ volatile("cpsie i" ::: "memory");
    return 0;
#endif
}

uint32_t plat_block_size(void)
{
    return 512;
}

uint32_t plat_flash_detected_bytes(void)
{
    return g_flash_detected;
}

uint32_t plat_block_count(void)
{
    /* The RAM disk is the filesystem here (fs_backing = embedded), and
     * ramdisk_init() grows it over the flash partition only on a
     * flash-backed board.  The count must follow the runtime-detected
     * capacity, exactly like the ESP32-C3 port. */
    return plat_lfs_blocks() * 8u;
}

/* ---- boot -------------------------------------------------------------- */

void plat_board_early(void)
{
    uint32_t khz;

    /* The ROM leaves the SAU enabled with eight regions (SAU_CTRL=1,
     * SAU_TYPE=8).  This is a Secure-only image: there is no Non-secure
     * world, no NSC veneer and no secure gateway, so any region that marks
     * a slice of SRAM/flash Non-secure only makes Secure instruction
     * fetches and branches land in an invalid-transition SecureFault.
     * Disabling the SAU with ALLNS=0 makes the whole address space Secure -
     * the same flat model the RP2040/M0+ port has.
     *
     * This has to happen before anything executes from SRAM, not after the
     * clock setup: pll_sys_init() switches clk_sys from a .data.ramfunc
     * stub, and with the SAU still enabled that fetch faulted on the Pico 2
     * - a silent death exactly at the clock switch, with every handler in
     * flash so nothing was ever printed (issue #7).  Measured on hardware:
     * the same mux move from the same SRAM address works once the SAU is
     * off. */
    mmio_write32(ARM_VTOR, XIP_BASE);
    mmio_write32(ARM_SAU_CTRL, 0u);
    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    /* clk_peri is OFF after reset (CLOCKS_CLK_PERI_CTRL_RESET == 0).  UART0,
     * IO_BANK0 and PADS_BANK0 ALL sit behind it.  Touching any of those
     * registers with the clock gated stalls the AHB fabric FOREVER - it
     * does not return garbage - which is exactly a silent boot with no LED
     * and no byte on the wire.  CLOCKS/RESETS/XOSC themselves need no
     * clk_peri, so the clock must be enabled before anything else here. */
    g_clk_hz = 12000000u;       /* conservative guess until FC0 measures */
    clocks_init();

    /* Release the peripherals the early output needs before using them:
     * IO_BANK0, PADS_BANK0 and UART0 are all held in reset on RP2350, and
     * touching them while they are costs an AHB stall, not a fault. */
    unreset(RESET_IO_BANK0 | RESET_PADS_BANK0 | RESET_UART0);

    /* Measure clk_sys before the first byte goes out and derive the UART
     * divider from the measurement, never from a guess.  The PL011 divider
     * comes from g_clk_hz, so any mismatch with the clock clk_peri is
     * actually running at turns every early line into garbage - which is
     * exactly how the Pico 2 bring-up looked: readable fragments, dropped
     * bytes, and a boot that appeared to die at the clock switch when it
     * had only lost its baud rate (issue #7). */
    khz = measure_clk_khz();
    g_clk_hz = (khz >= 1000u && khz <= 250000u) ? khz * 1000u
                                                : ARM_SYSTICK_CLK;
    uart_init();
#if TOS_BOOT_TRACE
    early_puts("\r\n[A] clocks+reset ok  clk_sys=");
    early_put_hex(g_clk_hz);
    early_puts("\r\n");
    /* Let the shift register drain before clk_peri changes: the tail of
     * this line would otherwise be clocked out at the new rate with the old
     * divisor and arrive as garbage. */
    plat_console_flush();
#endif

    /* From here on a fault is recoverable: the panic loop polls UART0 for
     * Ctrl-D, so a broken build never needs a physical BOOTSEL press. */
    g_panic_poll = panic_poll_uart;

    /* 150 MHz, or a no-op if the PLL refuses.  The switch itself runs from
     * SRAM (see clk_sys_to_pll_sys) and re-measures afterwards, because
     * clk_peri follows clk_sys and the UART divider has to follow it. */
    pll_sys_init();
    khz = measure_clk_khz();
    g_clk_hz = (khz >= 1000u && khz <= 250000u) ? khz * 1000u
                                                : ARM_SYSTICK_CLK;
    uart_init();
#if TOS_BOOT_TRACE
    early_puts("[B] clk=0x");
    early_put_hex(g_clk_hz);
    early_puts("\r\n");
#endif

    /* Arm the watchdog now that the clock is final: a kernel that wedges
     * later resets itself instead of waiting for a human with a button. */
    wdt_enable();

    led_init();
    led_set(1);
#if TOS_BOOT_TRACE
    early_puts("[C] vtor+sau+led ok\r\n");
#endif

    /* Defensive core-1 park: the ROM normally holds core 1 in its own
     * wait-for-FIFO loop, but a stray multicore launch must not execute
     * our single-core kernel image twice. */
    if (mmio_read32(SIO_CPUID) != 0) {
        for (;;)
            __asm__ volatile("wfe");
    }
#if TOS_BOOT_TRACE
    early_puts("[D] cpu0\r\n");

    early_puts("TinyOS/RP2350  clk=0x");
    early_put_hex(g_clk_hz);
    early_puts(" Hz  uart0=115200\r\n");
#endif
    /* Bring up the full console (UART re-divide + native USB CDC) the same
     * way every other ARM board does from plat_board_early().  Without this
     * call the CDC controller is never taken out of reset, no 2E8A COM port
     * enumerates, and g_usb_ready stays 0 forever - the kernel would have
     * exactly one wire.  usb_cdc_console_init() waits a bounded ~1.5 s for
     * SET_CONFIGURATION, so a board with no USB cable still boots on UART0.
     * Once the tick is live plat_timer_tick() blinks the LED every second;
     * before that a solid LED means we reached this point. */
    plat_console_init();
}

void plat_board_init(void)
{
    /* Resolve the ROM flash entries first: the capacity probe below (JEDEC
     * RDID through QMI direct mode) needs g_rom_connect/g_rom_exit_xip. */
    g_rom_connect   = (rp_rom_void_t)rp_rom_func(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    g_rom_exit_xip  = (rp_rom_void_t)rp_rom_func(ROM_FUNC_FLASH_EXIT_XIP);
    g_rom_enter_xip = (rp_rom_void_t)rp_rom_func(ROM_FUNC_FLASH_ENTER_CMD_XIP);
    g_rom_flush     = (rp_rom_void_t)rp_rom_func(ROM_FUNC_FLASH_FLUSH_CACHE);
    g_rom_erase     = (rp_rom_erase_t)rp_rom_func(ROM_FUNC_FLASH_RANGE_ERASE);
    g_rom_program   = (rp_rom_program_t)rp_rom_func(ROM_FUNC_FLASH_RANGE_PROGRAM);
    g_rom_ready = (g_rom_connect && g_rom_exit_xip && g_rom_enter_xip &&
                   g_rom_flush && g_rom_erase && g_rom_program) ? 1 : 0;

    /* Snapshot the CS0 flash capacity before anything else: this is the
     * first board code after the console, and the JEDEC RDID probe turns
     * XIP off for a moment (the other core is parked out of flash on SMP).
     * Kept in g_flash_detected so /proc/flash can show the true capacity
     * instead of the configured 4 MiB (ATK) / 8 MiB (Pico 2) upper bound. */
    g_flash_detected = rp2350_flash_detect();
#if TOS_BOOT_TRACE
    early_puts("[F] flash id/detected=");
    early_put_hex(g_flash_detected);
    early_puts("\r\n");
#endif

    /* The ARM USB controller receives IRQ 14, but TinyUSB's task loop still
     * belongs in thread context.  Install it here, before the scheduler
     * starts, so a blocked shell lets the idle loop run it. */
    g_idle_poll = usb_idle_poll;
}

/* ---- power -------------------------------------------------------------- */

void plat_shutdown(void)
{
    console_shutdown();

    /* console_shutdown() masked every interrupt, so plat_panic_halt()'s
       wfi would never wake and a dead kernel could only be recovered with a
       physical BOOT+RST press.  Keep polling UART0 for Ctrl-D instead: the
       RX FIFO works without interrupts, so a script can always put the ROM
       back in BOOTSEL, even after init has crashed. */
    for (;;) {
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE) &&
            (mmio_read32(RP_UART_DR) & 0xffu) == BOOTSEL_MAGIC_CHAR)
            rom_enter_bootsel();
        for (volatile int i = 0; i < 200; i++)
            ;
    }
}

void plat_reboot(void)
{
    console_shutdown();
    /* Go back through the ROM's own reboot entry with BOOT_TYPE_NORMAL: it
     * re-scans flash for the IMAGE_DEF and boots the same image, exactly the
     * path a power-on takes.  The AIRCR's SYSRESETREQ below is the fallback,
     * not the plan: it leaves the USB controller and the clock tree as they
     * were, and measured on this board the shell came back with a CDC port
     * the host could enumerate but not open, and no UART0 output at all -
     * i.e. it hung.
     *
     * The other core has to be parked first, exactly as rom_enter_bootsel()
     * does: the ROM reboot takes core 0 away, and a core 1 still running the
     * old kernel on the same physical addresses is a coin flip.  Measured on
     * rpi-pico2-smp: the first "reboot" came back, the second left a CDC port
     * that enumerated, opened and never answered - the same shape as the
     * AIRCR hang above.  g_core1_stopped keeps it in the park loop for good,
     * so there is nothing to unpark afterwards. */
    rom_stop_other_core();
    rom_reboot(0x100u);           /* BOOT_TYPE_NORMAL | NO_RETURN_ON_SUCCESS */
    mmio_write32(ARM_AIRCR, ARM_AIRCR_VECTKEY | ARM_AIRCR_SYSRESETREQ);
    plat_panic_halt();
}

uint32_t plat_board_cpu_id(void)
{
    return mmio_read32(SIO_CPUID);
}

const char *plat_cpu_model(void)
{
    return "Cortex-M33 (RP2350)";
}

uint32_t plat_cpu_hz(void)
{
    return g_clk_hz;
}

/* PMSAv8 describes a region with a base and an inclusive limit at 32-byte
 * granularity - no power-of-two rule - so the window can follow the C3's
 * shape: 4 KiB steps, 4 KiB default stack, and one 4 KiB of reserve for the
 * first stack doubling.  arch/arm/platform.c rounds wx and brk to 32 bytes and
 * emits code / data / stack as three separate areas, so .rodata stays RX and
 * the guard hole between brk and the stack stays uncovered. */
uint32_t plat_window_grow_reserve(void)
{
    return PLAT_STACK_BYTES;
}

int plat_window_needs_stack_alignment(void)
{
    return 0;
}

/* The MPU can bound a stack but it cannot grow one: Cortex-M exception entry
 * pushes its frame onto PSP, and a push that lands below stack_low fails
 * (MSTKERR) with no frame left to return to.  arch/arm/platform.c leaves a
 * writable landing strip just under the stack so that push succeeds; the
 * canary checked on every syscall is what notices the overrun and calls
 * process_stack_grow() before the next push has to go any deeper.  This is
 * the same mechanism the MPU-less ports use, not a second fault path. */
int plat_user_stack_guard(void)
{
    return 1;
}

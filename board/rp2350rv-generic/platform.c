#include "platform.h"
#include "board_config.h"

/* Park-skeleton hooks (issue #17).  Defined before rp_park.h so the shared
 * RP_PARK_OWNER_TAKE / RP_PARK_PARKED_LOOP macros pick them up.  They name
 * symbols declared later in this file, which is fine: a macro expands only at
 * its call site, well after those declarations.  See board/rp_park.h. */
#define RP_PARK_WDT_TICK(spins)                                              \
    do { if (((spins) & 0xfffffu) == 0xfffffu) wdt_feed(); } while (0)
#define RP_PARK_OWNER_TICK(spins) RP_PARK_WDT_TICK(spins)
/* Set before a ROM reboot: the parked hart must stay in the loop instead of
 * returning to a kernel that is about to be replaced under it.  Same hook the
 * ARM port uses for g_core1_stopped. */
#define RP_PARK_STOP_FOREVER()                                               \
    do { if (g_hart1_stopped) for (;;) wdt_feed(); } while (0)
#if TOS_DIAG
#define RP_PARK_PARKED_DIAG(entering)                                        \
    do {                                                                     \
        uint32_t _rp_d = (uint32_t)plat_cpu_id() & 1u;                       \
        if (entering) {                                                      \
            g_site[_rp_d] = 3;              /* parked */                     \
            g_pk_parker = _rp_d;                                             \
            g_park_enter[_rp_d]++;                                           \
            g_pk_phase = 2;                 /* acking */                     \
        }                                                                    \
    } while (0)
#define RP_PARK_OWNER_PREPARE()                                              \
    do {                                                                     \
        uint32_t _rp_d = (uint32_t)plat_cpu_id() & 1u;                       \
        g_pk_writer = (uint32_t)plat_cpu_id();                               \
        g_wr_enter[_rp_d]++;                                                 \
        g_wr_live |= 1u << _rp_d;                                            \
        g_pk_phase = 1;                     /* request raised */             \
        g_pk_req++;                                                          \
    } while (0)
#define RP_PARK_OWNER_ACKED()                                                \
    do {                                                                     \
        g_pk_ack++;                                                          \
        g_wr_live &= ~(1u << ((uint32_t)plat_cpu_id() & 1u));                \
    } while (0)
#define RP_PARK_OWNER_TIMEOUT()                                              \
    do {                                                                     \
        g_pk_to++;                           /* timeout */                   \
        g_wr_live &= ~(1u << ((uint32_t)plat_cpu_id() & 1u));                \
        g_pk_last_to = g_pk_phase;                                           \
        /* The far hart never answered.  Say so on the wire with its last    \
         * known state: a caller that sees only -1 cannot tell "slow" from    \
         * "dead" from "never scheduled". */                                 \
        {                                                                    \
            uint32_t _a = g_tk[1];                                           \
            for (volatile uint32_t _d = 0; _d < 300000u; _d++)               \
                ;                                                            \
            early_puts("\r\n[!pk] t1a=");                                    \
            early_put_hex(_a);                                               \
            early_puts(" t1b=");                                             \
            early_put_hex(g_tk[1]);                                          \
            early_puts(" we=");                                              \
            early_put_hex(g_wr_enter[0] << 16 | g_wr_enter[1]);              \
            early_puts(" wl=");                                              \
            early_put_hex(g_wr_live);                                        \
        }                                                                    \
        early_puts("\r\n[!pk] t0=");                                         \
        early_put_hex(g_tk[0]);                                              \
        early_puts(" t1=");                                                  \
        early_put_hex(g_tk[1]);                                              \
        early_puts(" s0=");                                                  \
        early_put_hex(g_site[0]);                                            \
        early_puts(" s1=");                                                  \
        early_put_hex(g_site[1]);                                            \
        early_puts(" pen0=");                                                \
        early_put_hex(g_park_enter[0]);                                      \
        early_puts(" pen1=");                                                \
        early_put_hex(g_park_enter[1]);                                      \
        early_puts(" req=");                                                 \
        early_put_hex(g_pk_req);                                             \
        early_puts(" ack=");                                                 \
        early_put_hex(g_pk_ack);                                             \
        early_puts(" wr=");                                                  \
        early_put_hex(g_pk_writer);                                          \
        early_puts(" park=");                                                \
        early_put_hex(g_pk_parker);                                          \
        early_puts("\r\n");                                                  \
    } while (0)
#else
#define RP_PARK_PARKED_DIAG(entering) do { (void)(entering); } while (0)
#define RP_PARK_OWNER_PREPARE() do { } while (0)
#define RP_PARK_OWNER_ACKED() do { } while (0)
#define RP_PARK_OWNER_TIMEOUT() do { } while (0)
#endif

#include "rp_park.h"
#include "board.h"
#include "board_internal.h"
#include "rp2350rv_memmap.h"
#include "rp2350_xosc.h"   /* XOSC is a chip fact shared with the ARM port */
#include "console.h"
#include "usb_cdc.h"
#include "string.h"

/* Same attribute kernel/cpu.h defines, repeated here rather than pulling that
 * header in: it declares __kernel_stack_top as uint8_t[], which clashes with
 * this file's char[] declaration. */
#define TOS_NOINIT __attribute__((section(".noinit")))

/* Code that must execute from SRAM: flash_exit_xip puts QSPI into
 * serial-command mode and an instruction fetch from flash then hangs; the
 * same is true between the clk_sys mux write and the QMI retime.  The linker
 * script copies .data.ramfunc to SRAM before kmain. */
#define RAMFUNC __attribute__((section(".data.ramfunc"), noinline))

static void wdt_feed(void);        /* defined with the RAM helpers below */
static void wdt_enable(void);
static void early_puts(const char *s);
static void early_put_hex(uint32_t v);
static uint32_t measure_clk_src_khz(uint32_t src);

/* measure_clk_khz() is the common case: clk_sys itself. */
static uint32_t measure_clk_khz(void)
{
    return measure_clk_src_khz(FC0_SRC_CLK_SYS);
}

/* Board: Raspberry Pi RP2350, Hazard3 RISC-V cores.
 *
 * Booting is much simpler than on the ARM side: there is no vector table
 * and no stage-2.  The boot ROM finds our PICOBIN IMAGE_DEF at flash offset
 * 0 (board/rp2350rv-generic/image_def.S), jumps to the ENTRY_POINT it carries, and
 * gets out of the way.  The kernel then runs in place from the XIP window
 * at 0x10000000, with .data/.bss in SRAM at 0x20000000.
 *
 * Console: UART0 on GPIO0/1 at 115200 (the CH343 port) and the on-chip USB
 * device as a CDC-ACM COM port, both live at once.  USB is the same TinyUSB
 * device stack the ARM image uses; the only difference is that Hazard3
 * delivers external interrupts through its own CSRs, so this port polls the
 * controller (usb_cdc_poll) instead of taking IRQ 14.  The tight loop in
 * usb_cdc_console_init answers enumeration; after that the 1 ms console
 * path drains it.
 *
 * Timer is the Hazard3 machine timer, whose mtime/mtimecmp live in the SIO
 * block (0xd0000000 + 0x1b0), not in a CLINT and not in the ARM timer.
 *
 * Flash reads go straight through XIP.  Writes and erases use the boot
 * ROM's flash API, looked up through the RISC-V table at 0x7dfa; the ROM
 * wrappers must run from SRAM because QSPI leaves command mode mid-call. */

/* ---- boot ROM ---------------------------------------------------------- */

typedef void *(*rv_rom_lookup_t)(uint32_t code, uint32_t flags);
typedef void (*rv_rom_void_t)(void);
typedef void (*rv_rom_erase_t)(uint32_t addr, size_t count,
                               uint32_t block_size, uint8_t block_cmd);
typedef void (*rv_rom_program_t)(uint32_t addr, const uint8_t *data, size_t count);
typedef void (*rv_rom_reboot_t)(uint32_t flags, uint32_t delay_ms,
                                uint32_t p0, uint32_t p1);

#define ROM_FUNC_CONNECT_INTERNAL_FLASH 0x4649u   /* 'I','F' */
#define ROM_FUNC_FLASH_EXIT_XIP        0x5845u   /* 'E','X' */
#define ROM_FUNC_FLASH_ENTER_CMD_XIP   0x5843u   /* 'C','X' */
#define ROM_FUNC_FLASH_FLUSH_CACHE     0x4346u   /* 'F','C' */
#define ROM_FUNC_FLASH_RANGE_ERASE    0x4552u   /* 'R','E' */
#define ROM_FUNC_FLASH_RANGE_PROGRAM   0x5052u   /* 'R','P' */
#define ROM_FUNC_REBOOT                0x4252u   /* 'R','B' */

/* Data-table lookup ('F','D' = 0x4446, the bootram FLASH_DEVINFO16 copy).
 * The ROM's data table is addressed through the separate data-table entry
 * at 0x7df8 (BOOTROM_RV_DATA_LOOKUP_PTR), NOT the 0x7dfa func-table
 * entry: the func entry returns a 16-bit jump address embedded in the
 * table (2-byte aligned, unusable as a pointer), while the data entry
 * returns a pointer to a 32-bit pointer literal in ROM, which itself
 * points at the live bootram copy - one more dereference than a function
 * lookup.  (pico-sdk bootrom_constants.h + bootrom.h rom_data_lookup_
 * inline().) */
#define ROM_DATA_FLASH_DEVINFO16_PTR 0x4446u   /* 'F','D' */
#define ROM_DATA_FLAG_DATA           0x0040u

/* bootram FLASH_DEVINFO bit fields (RP2350 OTP): bits 8:11 = CS0_SIZE;
 * capacity = 4096u << CS0_SIZE (pico-sdk flash_devinfo_size_to_bytes()).
 * With OTP BOOT_FLAGS0_FLASH_DEVINFO_ENABLE clear the ROM reports the
 * conservative default CS0_SIZE = 12 (16 MiB). */
#define FLASH_DEVINFO_CS0_SIZE_MASK  0x0f00u
#define FLASH_DEVINFO_CS0_SIZE_LSB   8

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

static rv_rom_void_t    g_rom_connect;
static rv_rom_void_t    g_rom_exit_xip;
static rv_rom_void_t    g_rom_enter_xip;
static rv_rom_void_t    g_rom_flush;
static rv_rom_erase_t   g_rom_erase;
static rv_rom_program_t g_rom_program;
static int              g_rom_ready;

/* The JEDEC RDID probe sits above the definitions of the copyout/XIP
 * helpers (they are RAMFUNC, section-placed below), so declare them here. */
static RAMFUNC void flash_init_boot2_copyout(void);
static RAMFUNC void flash_enable_xip_via_boot2(void);

/* RISC-V lookup: the entry at 0x7dfa is a 16-bit address of a jump
 * instruction (the code is embedded in the table, unlike ARM where the
 * table holds a pointer), and the second argument is RT_FLAG_FUNC_RISCV. */
static void *rp_rom_func(uint32_t code)
{
    rv_rom_lookup_t lookup =
        (rv_rom_lookup_t)(uintptr_t)(*(volatile uint16_t *)BOOTROM_RV_LOOKUP_PTR);

    if (!lookup)
        return 0;
    return lookup(code, RT_FLAG_FUNC_RISCV);
}

/* Issue JEDEC RDID (0x9F) through QMI direct mode and return the 3-byte
 * manufacturer/dev-id/capacity code as one 24-bit value, or 0 on failure.
 * The ROM cannot do this: FLASH_OP covers erase/program/read only, so the
 * chip select and the FIFO are driven by hand, exactly as pico-sdk's
 * flash_do_cmd_cs() does.
 *
 * This runs at boot with XIP live, so it uses the same ROM sequence the
 * write path uses: copy the ROM's XIP-setup routine out of bootram,
 * connect the QSPI pins, drop XIP, talk to the part in direct mode,
 * restore XIP.  All RAM-resident.  The RDID itself must be clocked as
 * 1 + 4 bytes (dummy cycles clock the ID out): send the opcode followed
 * by four zero bytes and read back five, keeping the FIFO in lockstep.
 *
 * A JEDEC part answering 0xff/0x00 reads as a missing or floating bus and
 * returns 0 (mirrors the ESP32-C3 port's flash_detect_from_rom()). */
static RAMFUNC uint32_t rp2350_flash_detect_jedec(void)
{
    uint8_t tx[5] = {0x9fu, 0x00u, 0x00u, 0x00u, 0x00u};
    volatile uint8_t rx[5];
    uint32_t id;

    flash_init_boot2_copyout();
    g_rom_connect();
    g_rom_exit_xip();

    /* Assert CS0n and enable direct mode. */
    mmio_write32(QMI_DIRECT_CSR,
                 QMI_DIRECT_CSR_EN | QMI_DIRECT_CSR_ASSERT_CS0N);

    /* Byte-by-byte FIFO exchange: each DIRECT_TX write clocks a byte in,
     * each DIRECT_RX read consumes the byte clocked by the previous write.
     * Poll for FIFO space and data; a missing part leaves RXEMPTY set, but
     * the exchange still drains because DIRECT_RX reads of an empty FIFO
     * return the last byte shifted (0xff on a floating bus). */
    for (uint32_t i = 0; i < 5u; i++) {
        while (mmio_read32(QMI_DIRECT_CSR) & QMI_DIRECT_CSR_TXFULL)
            ;
        mmio_write32(QMI_DIRECT_TX, tx[i]);
        while (mmio_read32(QMI_DIRECT_CSR) & QMI_DIRECT_CSR_RXEMPTY)
            ;
        rx[i] = (uint8_t)mmio_read32(QMI_DIRECT_RX);
    }

    /* Deassert CS0n and leave direct mode. */
    mmio_write32(QMI_DIRECT_CSR, 0u);

    /* First byte is the RDID echo (0x9f); bytes 1..3 are the JEDEC ID. */
    id = ((uint32_t)rx[1] << 16u) | ((uint32_t)rx[2] << 8u) | rx[3];
    if (id == 0u || id == 0xffffffu)
        id = 0u;

    flash_enable_xip_via_boot2();
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

    /* Fallback: the ROM's recorded FLASH_DEVINFO, via the data table.
     * The data-table entry (0x7df8) returns a pointer to a 32-bit pointer
     * literal in ROM; that literal points at the live 16-bit FLASH_DEVINFO
     * word in bootram.  Read it early so a later rewrite of the copy cannot
     * give a stale answer; mirrors the ESP32-C3 port's
     * flash_detect_from_rom(). */
    {
        rv_rom_lookup_t lookup =
            (rv_rom_lookup_t)(uintptr_t)(*(volatile uint16_t *)
                                         BOOTROM_RV_DATA_LOOKUP_PTR);
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
         * otherwise shrink the filesystem to nothing.  Mirrors the ESP32-C3
         * port's JEDEC capacity-code check. */
        if (size < 0x8u || size > 0xcu)
            return 0;
        return 4096u << size;
    }
}

static uint32_t g_flash_detected;

/* BOOTSEL on Ctrl-D: the host script sends 0x04 on UART0 and the ROM
 * re-enumerates the drive, so a broken image never needs a button press. */
#define BOOTSEL_MAGIC_CHAR 0x04u

static void rom_enter_bootsel(void)
{
    rv_rom_reboot_t reboot = (rv_rom_reboot_t)rp_rom_func(ROM_FUNC_REBOOT);

    if (reboot)
        reboot(0x102u, 10u, 0u, 0u);   /* BOOTSEL | NO_RETURN_ON_SUCCESS */
    for (;;)
        ;
}

static void rom_reboot_normal(void)
{
    rv_rom_reboot_t reboot = (rv_rom_reboot_t)rp_rom_func(ROM_FUNC_REBOOT);

    if (reboot)
        reboot(0x100u, 10u, 0u, 0u);   /* BOOT_TYPE_NORMAL */
}

/* ---- console: UART0 ---------------------------------------------------- */

#define UART_BAUD 115200u

/* clk_sys as measured after the PLL switch.  The UART divisor and the
 * machine-timer interval both come from here, never from a constant, so a
 * PLL that refuses to lock leaves a correct (if slower) board. */
static uint32_t g_clk_hz = 12000000u;

/* Generation counters for the flash-park handshake.  The boolean above only
 * says "some request is pending"; these say *which* one, so an
 * acknowledgement left over from the previous request can never be mistaken
 * for this one, and a hart that is still parked can acknowledge a new request
 * without leaving the RAM loop it is already inside.
 *
 * Defined outside the SMP conditional because the reset-surviving step
 * marker (flash_step) records them even in a uniprocessor build, where they
 * simply stay zero. */
static struct rp_park_state park = RP_PARK_STATE_INIT;

/* Set by plat_reboot() before the ROM takes core 0 away; see
 * RP_PARK_STOP_FOREVER above. */
static volatile int g_hart1_stopped;

#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
/* Set by core 0 before it takes XIP away for a ROM flash call; the parking
 * block next to plat_block_erase is what acts on it. */
/* PARK-FIX: which core called rp_flash_park_other().  The tick and idle
 * checks must never park the initiator - it is inside the thread wait that
 * is the only code which will ever clear the flag, and once its tick catches
 * the flag both harts spin in the RAM loop forever. */
static void rp_flash_park_loop(void);
#endif

/* Set once USB CDC came up and the host finished SET_CONFIGURATION.  Before
 * that, output must not be shunted into a FIFO nobody drains. */
static int g_usb_ready;

/* ---- flash-park / tick diagnostics (TOS_DIAG only) ---------------------
 * A dd write on atk-dnr2350am-rv32-smp intermittently came back with -1 ("the other hart
 * never parked") and, once, the other hart panicked with "no runnable
 * process" at the same moment.  Neither symptom says *why* the hart stopped
 * answering, so record the handshake as it happens, keep the record across a
 * reset (.noinit) and print it on the next boot, and shout on the wire the
 * moment a park request times out - while the other hart's state can still be
 * observed rather than inferred.
 *
 *   phase: 1 request raised   2 acked         3 XIP down
 *          4 ROM call returned 5 XIP restored  6 unparked
 *   site:  what a hart was inside when it last ran to a known point
 *          (1 tick handler, 2 idle/spin poll, 3 parked in the RAM loop). */
#if TOS_DIAG
TOS_NOINIT static uint32_t g_diag_magic0;
TOS_NOINIT static uint32_t g_diag_magic1;
TOS_NOINIT static uint32_t g_pk_req;
TOS_NOINIT static uint32_t g_pk_ack;
TOS_NOINIT static uint32_t g_pk_to;
TOS_NOINIT static uint32_t g_pk_phase;
TOS_NOINIT static uint32_t g_pk_last_to;      /* phase frozen at the last timeout */
TOS_NOINIT static uint32_t g_tk[2];           /* ticks actually delivered per hart */
TOS_NOINIT static uint32_t g_site[2];
TOS_NOINIT static uint32_t g_park_enter[2];
TOS_NOINIT static uint32_t g_pk_writer;
TOS_NOINIT static uint32_t g_pk_parker;
TOS_NOINIT static uint32_t g_wr_enter[2];     /* entries into the handshake */
TOS_NOINIT static uint32_t g_wr_live;         /* harts inside it right now */

#define DIAG_MAGIC0 0x544f5344u               /* "TOSD" */
#define DIAG_MAGIC1 0x31414744u               /* "1AGD" */
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

    /* GPIO0 = TX, GPIO1 = RX (FUNCSEL 2).  Clear ISO or the CH343 sees no
     * edges at all even though every register write succeeds. */
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

/* Console UART programming for /proc/uart0; word order documented in
 * plat_uart_debug() and identical on both RP2350 ports. */
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

/* Same self-test as the ARM port: transmit 0x55 and watch GP0 through
 * SIO_GPIO_IN for whole frames, with GP1 (the adapter's idle-high TX) and
 * GP25 (the LED) as control pins, plus an idle baseline taken before the
 * first byte. */
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
            return 0;
        mmio_write32(RP_UART_DR, 0x55u);
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

void plat_console_init(void)
{
    uart_init();
}

void plat_console_putc(char c)
{
    uint32_t spins;

    /* The PL011 FIFO must never stall the kernel forever: a board with no
     * adapter still has to reach the USB console. */
    for (spins = 0; spins < 100000u; spins++) {
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF))
            break;
    }
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

/* How long one usb_cdc_poll() takes, in nanoseconds (issue #29).
 *
 * The ARM ports bound their boot pump by iteration count (40 000 polls ~
 * 250 ms there).  The Hazard3 port cannot copy that number: the same bound
 * was a hung boot here, because its poll does more work per call (it moves the
 * tx ring itself, drains with usb_tx_drain() and takes spin locks).  Any
 * bounded pump has to start from a measured cost, and mtime is already the
 * tick's clock, so measure against it directly.
 *
 * Cost is measured over a fixed slice rather than a fixed count: a fixed count
 * would spend a whole slice of the boot on a fast part and almost nothing on a
 * slow one. */
static uint64_t mtime_read(void);        /* clock source for the measurement */

uint32_t plat_usb_poll_ns(void)
{
    uint64_t t0, t1;
    uint32_t n = 0;

    if (!g_usb_ready || !g_clk_hz)
        return 0;

    t0 = mtime_read();
    do {
        usb_cdc_poll();
        n++;
        t1 = mtime_read();
    } while (t1 - t0 < (uint64_t)g_clk_hz / 50u && n < 20000u);  /* ~20 ms */

    if (!n || t1 == t0)
        return 0;
    return (uint32_t)(((t1 - t0) / n) * 1000000000ull / (uint64_t)g_clk_hz);
}

int plat_console_tx_room(void)
{
    return (mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF) ? 0 : 16;
}

extern void (*g_idle_poll)(void);       /* kernel/process.c: idle hook */

/* Runs the polled USB controller's state machine from the scheduler's idle
 * loop: thread context, interrupts on.  See plat_console_getchar() for why it
 * must not run from the tick. */
static void usb_idle_poll(void)
{
    if (plat_cpu_id() == 0 && g_usb_ready)
        usb_cdc_poll();
}

int plat_console_getchar(void)
{
    int c;

    /* tty_poll() runs with interrupts masked and, on the tick path, inside
     * the timer interrupt.  TinyUSB's state machine does not belong there:
     * a retry loop inside tud_task() then holds the console lock forever and
     * the boot stops mid-line.  The state machine runs from the scheduler's
     * idle loop instead - real thread context, interrupts on (see
     * usb_idle_poll() above).  Only the byte queue is touched here. */
    if (plat_cpu_id() == 0 && g_usb_ready) {
        c = usb_cdc_getchar();
        if (c >= 0) {
            /* Ctrl-D over CDC: same ROM reboot as the UART path below. */
            if (c == BOOTSEL_MAGIC_CHAR)
                rom_enter_bootsel();
            return c;
        }
    }

    if (mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE)
        return -1;
    c = (int)(mmio_read32(RP_UART_DR) & 0xffu);
    if (c == BOOTSEL_MAGIC_CHAR)
        rom_enter_bootsel();
    return c;
}

/* Polled from plat_panic_halt(): the RX FIFO still works with interrupts
 * off, so a crashed image can always be pushed back into the ROM loader. */
void plat_board_panic_poll(void)
{
    if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE) &&
        (mmio_read32(RP_UART_DR) & 0xffu) == BOOTSEL_MAGIC_CHAR)
        rom_enter_bootsel();
}

static void early_puts(const char *s)
{
    while (*s)
        plat_console_putc(*s++);
}

static void early_put_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 28; i >= 0; i -= 4)
        plat_console_putc(hex[(v >> i) & 0xfu]);
}

/* ---- clock tree -------------------------------------------------------- */

static void unreset(uint32_t bits)
{
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) & ~bits);
    for (uint32_t i = 0; i < plat_pre_tick_spins(1000000u); i++) {
        if ((mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & bits) == bits)
            return;
    }
}

/* What the ROM handed us, captured before the first clock write and printed
 * in the [A] line.  The "just flashed" and "after RESET" boots are supposed
 * to differ in exactly these three registers, so this is the one place that
 * can tell the two apart. */
static uint32_t g_boot_sys_sel;
static uint32_t g_boot_ref_sel;
static uint32_t g_boot_xosc;

/* clk_ref = XOSC (12 MHz); clk_peri = clk_sys.  Two rules decide the order.
 *
 * First, the crystal has to be enabled before anything else: XOSC_CTRL
 * resets to DISABLE and a flash boot does not leave it running (measured on
 * hardware: XOSC_STATUS=0, clk_ref = ROSC), so PLL_USB would power up without
 * a reference and never lock.
 *
 * Second, never move the CPU onto a clock that has not just been seen
 * running.  The previous version followed pico-sdk's runtime_init_clocks
 * order - clk_sys onto clk_ref first, then clk_ref onto ROSC - and assumed
 * ROSC is always there.  If the ROM leaves ROSC stopped (the USB boot path
 * can), that second write routes the core onto a dead clock: no output, no
 * fault, nothing until a RUN/power reset brings ROSC back.  That is the shape
 * of "after flashing the RV image UART0 is silent until RESET".
 *
 * So the order is the ARM port's, which is proven on this board in both boot
 * states: crystal up, clk_ref onto the crystal once STABLE is set, and only
 * then clk_sys onto clk_ref.  If the crystal never starts, clk_ref is left
 * exactly as the ROM left it - a source the CPU is demonstrably running from
 * - and the board boots slowly instead of not at all. */
/* See the ARM port (board/rp2350-generic/platform.c): wait for the crystal,
 * bounded, then bring it up the pico-sdk way - STARTUP.DELAY before ENABLE
 * when we are the one enabling it, a full startup period after STABLE when
 * the ROM already did. */
static void clocks_init(void)
{
    g_boot_sys_sel = mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET);
    g_boot_ref_sel = mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET);
    g_boot_xosc    = mmio_read32(XOSC_STATUS);

    if (rp2350_xosc_start() &&
        !(mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & (1u << 2))) {
        mmio_write32(CLOCKS_BASE + CLK_REF_CTRL_OFFSET, CLK_REF_CTRL_SRC_XOSC);
        for (uint32_t i = 0; i < plat_pre_tick_spins(1000000u); i++) {
            if (mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & (1u << 2))
                break;
        }
    }

    /* clk_ref is a running source at this point - the crystal just proved it,
     * or it is whatever the ROM was already feeding the core from - so taking
     * clk_sys off its aux mux cannot stop the CPU.  Placing clk_sys on
     * clk_ref before pll_sys_init() resets PLL_SYS is what makes that reset
     * safe. */
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET, 0u);
    for (uint32_t i = 0; i < plat_pre_tick_spins(1000000u); i++) {
        if (mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET) & 1u)
            break;
    }

    /* TIMER0 is programmed by ticks_init() once it is out of reset. */

    /* clk_peri = clk_sys, undivided.  AUXSRC 0 is clk_sys on the RP2350. */
    mmio_write32(CLOCKS_BASE + CLK_PERI_DIV_OFFSET, CLK_DIV_1);
    mmio_write32(CLOCKS_BASE + CLK_PERI_CTRL_OFFSET,
                 CLK_PERI_CTRL_ENABLE |
                 (CLK_PERI_CTRL_AUXSRC_CLK_SYS << 5));
}

/* The 64-bit TIMER (TIMERAWL, and the RV32 mtime it feeds) only counts while
 * the TICKS block emits its 1 MHz tick: TIMER0_CTRL.ENABLE plus TIMER0_CYCLES
 * = clk_ref/clk_tick - 1.  The ROM sets this up for the clock it booted with,
 * and only its flash boot releases TIMER0 from reset at all, so after a RUN
 * reset the counter is stopped: the scheduler never ticks, the console ring is
 * never drained and every driver delay spins its safety bound.
 *
 * This has to run *after* TIMER0 is out of reset (plat_board_early() does that)
 * and after clk_ref is final.  Programming it while the block is still held in
 * reset is a write into a block that may drop it.
 *
 * A slightly wrong tick rate beats a stopped counter: with no fallback the
 * board behaves exactly like the bug this fixes, so ROSC (no crystal, or a
 * crystal that failed to start) still gets a divisor, from the datasheet's
 * nominal rate, instead of having the timer switched off. */
static void ticks_init(void)
{
    uint32_t khz;

    if (mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & (1u << 2))
        khz = 12000u;           /* clk_ref = XOSC 12 MHz                  */
    else
        khz = 6500u;            /* clk_ref = ROSC: nominal, drifts, counts */

    mmio_write32(TICKS_BASE + TICKS_TIMER0_CYCLES_OFFSET, khz / 1000u - 1u);
    mmio_write32(TICKS_BASE + TICKS_TIMER0_CTRL_OFFSET, 1u);
}

/* Move clk_sys onto PLL_SYS from SRAM.  Same reasoning as the ARM port:
 * measured on the Pico 2 with a standalone harness, the ROM's own QMI timing
 * (EBh quad, CLKDIV 3 / RXDELAY 50) still reads flash correctly at 150 MHz,
 * and writing a different M0_TIMING in this step is what breaks XIP.  So the
 * mux is the only thing that moves; no QMI register is touched.
 *
 * Must run from SRAM so that no flash fetch happens between the mux write
 * and the point where the new clock is acknowledged. */
static RAMFUNC void clk_sys_to_pll_sys(void)
{
    uint32_t i;

    /* pico-sdk's clock_configure_internal() order: the glitchless mux is on
     * clk_ref (pll_sys_init() put it there), then AUXSRC is programmed, then
     * the glitchless source moves last.  Setting both in one write passes a
     * glitch when the aux mux changes. */
    mmio_write32(CLOCKS_BASE + CLK_SYS_DIV_OFFSET, CLK_DIV_1);
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET,
                 CLK_SYS_CTRL_AUX_PLL_SYS << 5);
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET,
                 CLK_SYS_CTRL_SRC_AUX |
                 (CLK_SYS_CTRL_AUX_PLL_SYS << 5));

    /* A fixed spin count keeps this off plat_pre_tick_spins(), which is
     * flash. */
    for (i = 0; i < 100000u; i++) {
        if (mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET) & (1u << 1))
            break;
    }
}

/* PLL_SYS at 150 MHz.  Failure is benign: the boot continues on clk_ref
 * (12 MHz) and every divisor is re-derived from the measured value.
 *
 * The safety rule is in clocks_init(): clk_sys is already on clk_ref before
 * this runs, and clk_ref is a source the CPU has just been running from, so
 * resetting PLL_SYS cannot stop the core.  The RISC-V boot ROM is allowed to
 * leave clk_sys on PLL_SYS, which is why the move off the aux mux happens
 * before the reset rather than here. */
static void pll_sys_init(void)
{
    uint32_t i;

    /* The dividers below are the 12 MHz crystal's.  If a cold start left
     * clk_ref on the ROSC (~6.5 MHz, drifting), the same dividers produce
     * ~81 MHz instead of 150 MHz and the flash timing the ROM set up no
     * longer matches; the boot then dies at the switch (measured on the
     * Pico 2: first boot after power-on, retry with a warm crystal was
     * fine).  Require the crystal as clk_ref before raising anything; the
     * boot continues at clk_ref with measured divisors instead. */
    if (!(mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & (1u << 2)))
        return;

    /* Defensive repeat: if this is ever called on its own, moving clk_sys
     * to the glitchless source is what makes the reset below safe. */
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET, 0u);
    for (i = 0; i < plat_pre_tick_spins(1000000u); i++) {
        if (mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET) & 1u)
            break;
    }

    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) | RESET_PLL_SYS);
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) & ~RESET_PLL_SYS);
    for (i = 0; i < plat_pre_tick_spins(1000000u); i++) {
        if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & RESET_PLL_SYS)
            break;
    }

    PLL_SYS_CS_REG    = 1u;          /* REFDIV  */
    PLL_SYS_FBDIV_REG = 125u;        /* VCO = 1500 MHz */
    PLL_SYS_PRIM_REG  = (5u << 16) | (2u << 12);
    PLL_SYS_PWR_REG  &= ~(PLL_SYS_PWR_PD | PLL_SYS_PWR_VCOPD);

    for (i = 0; i < 2000000u; i++) {
        if (PLL_SYS_CS_REG & PLL_SYS_CS_LOCK)
            break;
    }
    if (!(PLL_SYS_CS_REG & PLL_SYS_CS_LOCK))
        return;

    PLL_SYS_PWR_REG &= ~PLL_SYS_PWR_POSTDIVPD;

    /* 150 MHz, but only if the PLL is really there - same reason as the ARM
     * port: on a cold start the crystal is still drifting when LOCK asserts,
     * and switching clk_sys onto a drifting PLL hangs the first boot.  Keep
     * clk_sys on clk_ref when the measured output is not 150 MHz; the board
     * then boots at 12 MHz and the next reset gets the full rate. */
    if (measure_clk_src_khz(FC0_SRC_PLL_SYS) < 149000u ||
        measure_clk_src_khz(FC0_SRC_PLL_SYS) > 151000u)
        return;

    /* Switch clk_sys onto the PLL from SRAM; see clk_sys_to_pll_sys(). */
    clk_sys_to_pll_sys();
}

/* Measure clk_sys in kHz against the 12 MHz crystal.  Returns 0 when the
 * counter does not settle; the caller then keeps its conservative default. */
static uint32_t measure_clk_src_khz(uint32_t src)
{
    uint32_t spins;
    const uint32_t bound = plat_pre_tick_spins(1000000u);

    for (spins = 0; spins < bound; spins++) {
        if (!(mmio_read32(FC0_STATUS) & FC0_STATUS_RUNNING))
            break;
    }
    /* FC0 measures against clk_ref, which is the 12 MHz crystal only when
     * the mux actually selected it.  clocks_init() falls back to the ROSC
     * (~6.5 MHz) when the crystal does not start, and assuming 12 MHz then
     * scales every measurement - and the UART divisor derived from it - by
     * the ratio between the two.  Read the mux instead of assuming. */
    mmio_write32(FC0_REF_KHZ,
                 (mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) &
                  (1u << 2)) ? 12000u : 6500u);
    mmio_write32(FC0_MIN_KHZ, 0u);
    mmio_write32(FC0_MAX_KHZ, 0xffffffffu);
    mmio_write32(FC0_INTERVAL, 10u);
    mmio_write32(FC0_SRC, src);

    for (spins = 0; spins < bound; spins++) {
        if (mmio_read32(FC0_STATUS) & FC0_STATUS_DONE)
            break;
    }
    if (spins == bound)
        return 0;
    return mmio_read32(FC0_RESULT) >> FC0_RESULT_KHZ_S;
}

/* ---- LED --------------------------------------------------------------- */

static void led_init(void)
{
    uint32_t pad = mmio_read32(RP_GPIO_PAD(RP_LED_GPIO));

    mmio_write32(RP_GPIO_CTRL(RP_LED_GPIO), RP_GPIO_FUNC_SIO);
    mmio_write32(RP_GPIO_PAD(RP_LED_GPIO),
                 (pad | RP_PAD_IE) & ~(RP_PAD_OD | RP_PAD_ISO));
    mmio_write32(SIO_GPIO_OUT_SET, 1u << RP_LED_GPIO);  /* off (active low) */
    mmio_write32(SIO_GPIO_OE_SET, 1u << RP_LED_GPIO);
}

static void led_toggle(void)
{
    static int on;

    on = !on;
    mmio_write32(on ? SIO_GPIO_OUT_CLR : SIO_GPIO_OUT_SET, 1u << RP_LED_GPIO);
}

/* ---- machine timer ----------------------------------------------------- */

static volatile uint64_t g_ticks;
static uint32_t g_interval;

static uint64_t mtime_read(void)
{
    uint32_t hi1, lo, hi2;

    do {
        hi1 = mmio_read32(SIO_MTIMEH);
        lo  = mmio_read32(SIO_MTIME);
        hi2 = mmio_read32(SIO_MTIMEH);
    } while (hi1 != hi2);
    return ((uint64_t)hi1 << 32) | lo;
}

static void mtime_set_cmp(uint64_t v)
{
    /* The high word is parked at all-ones while the low word changes, so a
     * write cannot look like an already-expired deadline and fire the
     * interrupt in the middle of the update. */
    mmio_write32(SIO_MTIMECMPH, 0xFFFFFFFFu);
    mmio_write32(SIO_MTIMECMP, (uint32_t)v);
    mmio_write32(SIO_MTIMECMPH, (uint32_t)(v >> 32));
}

static void timer_unmask(void)
{
    uint32_t mie;

    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    mie |= (1u << 7);                       /* MTIE: machine timer */
    __asm__ volatile("csrw mie, %0" : : "r"(mie));
}

void plat_timer_init(uint32_t hz)
{
    g_interval = g_clk_hz / hz;
    if (g_interval == 0)
        g_interval = 1;
    g_ticks = 0;

    /* Stop and reset the counter first: the ROM may have left it running
     * at a rate nobody documented. */
    mmio_write32(SIO_MTIME_CTRL, 0u);
    mmio_write32(SIO_MTIME, 0u);
    mmio_write32(SIO_MTIMEH, 0u);
    mtime_set_cmp(0xFFFFFFFFFFFFFFFFull);

    mtime_set_cmp(mtime_read() + g_interval);
    timer_unmask();
    mmio_write32(SIO_MTIME_CTRL,
                 SIO_MTIME_CTRL_EN | SIO_MTIME_CTRL_FULLSPEED);
}

void plat_timer_start_cpu(void)
{
    if (g_interval == 0)
        return;
    mtime_set_cmp(mtime_read() + g_interval);
    timer_unmask();
}

uint64_t plat_ticks(void)
{
    return g_ticks;
}

uint32_t plat_tick_hz(void)
{
    return g_interval ? g_clk_hz / g_interval : 0;
}

uint32_t plat_uptime_ms(void)
{
    /* 32-bit arithmetic only: a 64/32 variable divide would pull in
     * compiler-rt, and the RV32 kernel links with -nostdlib.  At 1 kHz this
     * wraps after 49 days, the same as the ARM port. */
    uint32_t hz = plat_tick_hz();

    return hz ? ((uint32_t)g_ticks * 1000u) / hz : 0;
}

uint64_t plat_now_ns(void)
{
    uint64_t t = mtime_read();
    uint64_t hz = g_clk_hz ? g_clk_hz : 1u;

    /* mtime counts clk_sys cycles with FULLSPEED.  Both divisions are
     * variable, so this is the one place on the port that pulls in
     * kernel/libgcc.c's 64-bit helpers - which exist; the earlier version
     * hardcoded 150 MHz and made clock_ms() run 12.5x fast at the ROM's
     * 12 MHz clock.  Split before scaling so a long uptime cannot overflow. */
    return (t / hz) * 1000000000ull + ((t % hz) * 1000000000ull) / hz;
}

void plat_timer_tick(void)
{
#if TOS_DIAG
    g_tk[(uint32_t)plat_cpu_id() & 1u]++;      /* tick actually delivered */
    g_site[(uint32_t)plat_cpu_id() & 1u] = 1;
#endif
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    /* Core 0 is about to turn XIP off for a ROM flash call: park here, in
     * RAM, before the next instruction fetch can touch flash.  Never park
     * the hart that initiated the flash call. */
    if (rp_park_should_park(&park, (int)plat_cpu_id()))
        rp_flash_park_loop();
#endif
    /* Only the boot CPU counts time.  With one hart this is always true; it
     * is written this way so an SMP build would not run the clock at 2x. */
    if (plat_cpu_id() == 0) {
        g_ticks++;
        if ((g_ticks % 50u) == 0u)
            wdt_feed();          /* see the watchdog note above */
        /* Keep the CDC FIFO fed from the ring.  tuD's state machine stays in
         * thread context; this only moves bytes, which is safe here. */
        if (g_usb_ready)
            usb_cdc_tx_kick();
    }
    mtime_set_cmp(mtime_read() + g_interval);

    if (plat_cpu_id() == 0 && (g_ticks % 1000u) == 0u && !plat_led_owned())
        led_toggle();
}

/* Hazard3 reports the machine timer as cause 7, the standard
 * M-mode timer interrupt. */
int plat_irq_is_tick(uint32_t line)
{
    return line == 7u;
}

#if TOS_RV_USB_IRQ
/* mcause 11 = machine external.  Read the XH3IRQ pending array and hand the
 * USB line to the same task the polled path runs, but immediately: the
 * control-transfer timing needs it.  Returning 1 tells the trap handler the
 * line was ours; anything else falls through to its panic. */
int plat_irq_foreign(uint32_t line)
{
    uint32_t pend;

    if (line != 11u)
        return 0;
    __asm__ volatile("csrr %0, 0xbe1" : "=r"(pend));
    if (!(pend & (1u << 14u)))
        return 0;
    usb_cdc_irq_task();
    return 1;
}
#else
/* With TOS_RV_USB_IRQ off, arch/rv32's `extern ... __attribute__((weak))`
 * reference has no definition - and under RV32 medany that does not link
 * to 0 as on ARM, but to a jump-to-self thunk, defeating its own `if (cb)`
 * null check the first time a non-tick line shows up.  Define it for real. */
int plat_irq_foreign(uint32_t line)
{
    (void)line;
    return 0;
}
#endif

void plat_irq_service(uint32_t line)
{
    (void)line;
}

/* ---- block device: NOR flash, or a RAM disk when the board has none ------ */

/* Reads go through the XIP window at 0x10000000; the littlefs partition is
 * at flash offset 0x80000 (PLAT_LFS_LBA * 512), exactly like the ARM image.
 * An `fs_backing = embedded` board instead carries the same image in
 * .data.ramdisk and uses that buffer as the whole device. */

extern uint8_t __ramdisk_start[];
extern uint8_t __ramdisk_end[];

int plat_block_read(uint32_t lba, uint32_t count, void *buf)
{
#if TOS_HAS_FLASH
    uint32_t off = lba * 512u;
    uint32_t len = count * 512u;

    if (lba != 0 && off / 512u != lba)                  /* overflow */
        return -1;
    if (off + len < off || off + len > 0x1000000u)      /* past 16 MiB */
        return -1;
    memcpy(buf, (const void *)(XIP_BASE + off), len);
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

/* These two must execute from SRAM: flash_exit_xip puts QSPI into
 * serial-command mode and an instruction fetch from flash then hangs.  The
 * linker script copies .data.ramfunc to SRAM before kmain. */
#define RAMFUNC __attribute__((section(".data.ramfunc"), noinline))

/* ---- watchdog ----------------------------------------------------------
 * A wedge in this kernel is silent: both harts sit in a spin loop, no tick
 * runs, and only a physical reset brings the board back - the ROM's UART
 * bootloader cannot even be entered because the hart no longer reacts to
 * Ctrl-D.  The watchdog turns that into an automatic restart: the tick feeds
 * it while the system is alive, so a frozen tick stops feeding and the board
 * resets itself.
 *
 * RP2350 puts the watchdog at 0x400d8000.  The RP2040 address (0x40058000)
 * is PLL_USB on this chip, so the constant must not be copied over. */
#define WATCHDOG_BASE     0x400d8000u
#define WATCHDOG_CTRL_OFF 0x00u
#define WATCHDOG_LOAD_OFF 0x04u
#define WATCHDOG_CTRL_ENABLE  (1u << 30)
#define WATCHDOG_RELOAD   0x00ffffffu
/* SCRATCH0..7 are cleared only by a power-on reset, so a value written just
 * before a wedge tells the next boot exactly which step it died in - the one
 * piece of evidence a .noinit record cannot give when the reset comes from
 * the watchdog while the flash window has interrupts and the console off. */
#define WATCHDOG_SCRATCH(n) (WATCHDOG_BASE + 0x08u + 4u * (n))

/* Pinned to the RAM section rather than left to the inliner: wdt_feed() is
 * called from inside the park loop and wdt_enable() from inside the flash
 * window, both while XIP is off.  They were inlined into their RAMFUNC
 * callers in every build so far, which is exactly the kind of luck that turns
 * into a dead board the first time the optimiser makes a different choice. */
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

#if TOS_DIAG
/* ---- flash-park / tick diagnostics ------------------------------------- */
static void diag_kv(const char *k, uint32_t v)
{
    early_puts(k);
    early_put_hex(v);
}

static void diag_prev_report(void)
{
    if (g_diag_magic0 == DIAG_MAGIC0 && g_diag_magic1 == DIAG_MAGIC1) {
        early_puts("\r\n[PREV] phase=");
        early_put_hex(g_pk_last_to);
        diag_kv(" req=", g_pk_req);
        diag_kv(" ack=", g_pk_ack);
        diag_kv(" to=", g_pk_to);
        diag_kv(" t0=", g_tk[0]);
        diag_kv(" t1=", g_tk[1]);
        diag_kv(" s0=", g_site[0]);
        diag_kv(" s1=", g_site[1]);
        diag_kv(" p0=", g_park_enter[0]);
        diag_kv(" p1=", g_park_enter[1]);
        early_puts("\r\n");
    }
    g_pk_req = g_pk_ack = g_pk_to = 0;
    g_pk_phase = 0;
    g_pk_last_to = 0;
    for (int i = 0; i < 2; i++) {
        g_tk[i] = 0;
        g_site[i] = 0;
        g_park_enter[i] = 0;
    }
    g_diag_magic0 = DIAG_MAGIC0;
    g_diag_magic1 = DIAG_MAGIC1;
}

/* Last-step marker that survives any reset (WATCHDOG_SCRATCH).  Must run from
 * RAM: it is called inside the flash window. */
static RAMFUNC void flash_step(uint32_t code)
{
    mmio_write32(WATCHDOG_SCRATCH(0), code);
    mmio_write32(WATCHDOG_SCRATCH(1), park.req);
    mmio_write32(WATCHDOG_SCRATCH(2), g_tk[1]);
}
#else
#define flash_step(code) do { } while (0)
#endif

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

extern volatile int cpu_count;

/* While the ROM has QMI in direct mode, any XIP access from the other core is
 * a bus fault, so the secondary core has to be off flash before the first
 * byte of the sequence.  Same scheme as the RP2040 port: a flag, a RAM-resident
 * spin loop, a check in the tick (which preempts whatever core 1 is running)
 * and a check in idle (a core already inside its tick cannot be preempted
 * again, so the tick check alone would never see the flag). */
/* RAM-resident: this runs while the owner has XIP turned off.  The shared
 * loop re-publishes the ack for every generation (littlefs issues several
 * block operations back to back; a publish-once ack is the intermittent
 * "dd: write error at block 0 (-1)" on atk-dnr2350am-rv32-smp) and feeds the watchdog,
 * because this hart is parked with interrupts masked and the tick cannot. */
static RAMFUNC void rp_flash_park_loop(void)
{
    RP_PARK_PARKED_LOOP(&park, plat_pre_tick_spins(200000000u));
}

void plat_flash_park_poll(void)
{
#if TOS_DIAG
    g_site[(uint32_t)plat_cpu_id() & 1u] = 2;  /* idle/spin poll */
#endif
    if (rp_park_should_park(&park, (int)plat_cpu_id())) {
        /* Mask interrupts BEFORE publishing the acknowledgement.  The park
         * loop tells the writer "I am in RAM now"; anything that preempts
         * this core after that point resumes in flash while the writer has
         * QMI in serial command mode, and the fetch bus-faults inside an
         * exception whose handler is also in flash - a silent dead core.
         * The tick path is already masked; the idle and spin-wait callers
         * reach here with interrupts on. */
        uint32_t irq = plat_irq_save();

        rp_flash_park_loop();
        plat_irq_restore(irq);
    }
}

/* Mask interrupts for the handshake: with them on, the tick can land in this
 * wait loop and reschedule the caller onto the other hart, which then is no
 * longer the one that will unpark - both harts park and the flag is never
 * cleared.  The caller saved mstatus.MIE and restores it on both paths, so
 * RP_PARK_OWNER_RESTORE_IRQ is a no-op here. */
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

    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(was_enabled));
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    if (plat_flash_exclusive_begin() != 0) {
        /* plat_flash_exclusive_begin() leaves MIE masked on success because the
         * ROM call follows; on timeout it returns without parking anyone,
         * so the caller must put the original state back or every tick and
         * console interrupt stays off for the rest of the boot. */
        if (was_enabled & 8u)
            __asm__ volatile("csrsi mstatus, 8" ::: "memory");
        return -1;
    }
#endif
    /* Feed around the ROM call rather than disarming the watchdog: with it
     * off, a wedge inside the sequence was silent and permanent.  One erase
     * burst is ~50 ms against a 16 s reload. */
    wdt_feed();
    flash_step(0x101);
    flash_init_boot2_copyout();
    g_rom_connect();
#if TOS_DIAG
    g_pk_phase = 3;                          /* XIP going down */
#endif
    flash_step(0x102);
    g_rom_exit_xip();
    flash_step(0x103);
    g_rom_erase(addr, end - addr, RP_FLASH_SECTOR, RP_SECTOR_ERASE_CMD);
    flash_step(0x104);
    g_rom_flush();
#if TOS_DIAG
    g_pk_phase = 4;                          /* ROM call returned */
#endif
    wdt_enable();
    flash_step(0x105);
    flash_enable_xip_via_boot2();
#if TOS_DIAG
    g_pk_phase = 5;                          /* XIP restored */
#endif
    flash_step(0x106);
    plat_flash_exclusive_end();
#if TOS_DIAG
    g_pk_phase = 6;                          /* unparked */
#endif
    flash_step(0x107);
    if (was_enabled & 8u)
        __asm__ volatile("csrsi mstatus, 8" ::: "memory");
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

    addr = lba * 512u;

    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(was_enabled));
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    if (plat_flash_exclusive_begin() != 0) {
        if (was_enabled & 8u)
            __asm__ volatile("csrsi mstatus, 8" ::: "memory");
        return -1;
    }
#endif
    wdt_feed();                 /* stay armed; see the watchdog note above */
    flash_step(0x201);
    flash_init_boot2_copyout();
    g_rom_connect();
#if TOS_DIAG
    g_pk_phase = 3;                          /* XIP going down */
#endif
    flash_step(0x202);
    g_rom_exit_xip();
    flash_step(0x203);
    g_rom_program(addr, (const uint8_t *)buf, count * 512u);
    flash_step(0x204);
    g_rom_flush();
#if TOS_DIAG
    g_pk_phase = 4;                          /* ROM call returned */
#endif
    wdt_enable();
    flash_step(0x205);
    flash_enable_xip_via_boot2();
#if TOS_DIAG
    g_pk_phase = 5;                          /* XIP restored */
#endif
    flash_step(0x206);
    plat_flash_exclusive_end();
#if TOS_DIAG
    g_pk_phase = 6;                          /* unparked */
#endif
    flash_step(0x207);
    if (was_enabled & 8u)
        __asm__ volatile("csrsi mstatus, 8" ::: "memory");
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
    /* The filesystem is a flash-backed littlefs on the boards that carry
     * one (ATK 4 MiB, Pico 2 8 MiB) and a RAM disk elsewhere; the block
     * count must follow the runtime-detected capacity, exactly like the
     * ESP32-C3 port. */
    return plat_lfs_blocks() * 8u;
}

/* ---- user-mode PMP ----------------------------------------------------- */

/* Hazard3 on the RP2350 supports NAPOT and NA4 but not TOR: pmpcfg A=1 is
 * WARL-mapped to OFF, so arch/rv32/platform.c's TOR regions never match and
 * every user fetch faults at the window base.
 *
 * A window is not rounded up to a power of two.  The image+data range and the
 * stack are each covered by as many NAPOT blocks as they need (largest
 * aligned block first), the guard hole between them is deliberately left
 * uncovered, and entry 5 stays the trap vector.  A typical window needs 3-5
 * of the eight software-writable regions.
 *
 * The first page(s), which hold nothing but .text and .rodata, stay R+X so a
 * write to rodata still faults.  Everything from the page containing wx on is
 * R+W+X.  A full page-granular W^X split is NOT possible with the ELF layout
 * the loader produces: wx and brk routinely land inside the same 4 KiB page
 * (measured on the board: wx=0x2001d420, brk=0x2001e000).  Rounding the cut
 * DOWN puts the tail of .text into the RW/no-X region and the shell dies on
 * its first instruction (this is exactly what 4ef146c did); rounding it UP
 * puts .data/.bss into the RX/no-W region instead.  Hazard3 has no TOR, so
 * the overlapping regions needed for a permission union are not expressible
 * either.  Measured on the part: widening only entry 0 does not help, while
 * giving the page that holds wx the X bit boots reliably.
 *
 * What survives is the protection that matters at a window boundary: a task
 * cannot reach the kernel, another task, flash or MMIO, and the guard hole
 * still turns a heap overrun or a stack underrun into a fault.  rodata is NOT
 * reliably protected: when wx falls inside the first page (small images like
 * `prot`), ro_end collapses to win_base and the whole image lands in the RWX
 * part, so `prot rodata` survives.  Full W^X and rodata protection need the
 * linker to put wx on a page boundary - see OPEN-ITEMS §31.0.
 *
 * The budget, not alignment, is the limit: a window whose pieces fragment
 * into more than eight blocks cannot be described. */
#define PMP_NAPOT 0x18u
#define PMP_R     0x01u
#define PMP_W     0x02u
#define PMP_X     0x04u
/* RP2350 gives Hazard3 eight software-writable regions (0..7); 8..10 are
 * hardwired by the bootrom (ROM and peripherals, RWX) and 11..15 hardwired
 * OFF, so writing those is a no-op - they are not budget. */
#define PMP_ENTRIES   8
#define PMP_VEC_ENTRY 5
#define PMP_PAGE  0x1000u
extern char trap_vectors[];

static void pmp_program(const uint32_t *addr, const uint32_t *cfg);

static uint32_t napot(uint32_t base, uint32_t size)
{
    return (base >> 2) | ((size >> 3) - 1u);
}

/* Cover [base, top) with NAPOT blocks, recording entry numbers in addr[] and
 * cfg[].  Returns the next free entry, or -1 when the range cannot be named.
 *
 * Every block is at least one page and naturally aligned, and a block never
 * spills past top: overshooting would cover the guard hole below the stack and
 * a stack overflow would stop faulting.  Blocks are page-granular - measured
 * on the part, sub-page NAPOT blocks do not describe the intended range. */
static int pmp_cover(uint32_t base, uint32_t top, uint32_t perm,
                     uint32_t *addr, uint32_t *cfg, int next)
{
    while (base < top) {
        uint32_t rem = top - base;
        uint32_t size = 1u << 20;

        if (next == PMP_VEC_ENTRY)
            next++;
        if (next >= PMP_ENTRIES)
            return -1;
        while (size > PMP_PAGE &&
               (size > rem || (base & (size - 1u)) != 0))
            size >>= 1;
        if (size > rem || (base & (size - 1u)) != 0)
            return -1;                 /* tail not representable */
        addr[next] = napot(base, size);
        cfg[next] = perm | PMP_NAPOT;
        next++;
        base += size;
    }
    return next;
}

/* Attributes per page, derived from the segment list.
 *
 * A page that holds code only stays R+X; any page that also touches data or
 * the stack becomes R+W+X, because Hazard3 cannot express a permission union
 * and the page cannot be cut - and that RWX page is exactly the known
 * limitation (OPEN-ITEMS 31.0): wx usually lands inside the same page that
 * holds the start of .data.  The guard hole is not a segment, so its pages
 * are simply absent and stay uncovered, which is what keeps `prot heap` and
 * `prot stack` faulting.
 *
 * The map is one page per byte, and a window can never exceed the whole task
 * area (96 pages at the 4 KiB grain), so a fixed buffer keeps this off the
 * heap - this runs on every task switch. */
#define PMP_PAGE_CODE  0x1u
#define PMP_PAGE_DATA  0x2u
#define PMP_MAX_PAGES  128

static int pmp_page_attrs(const plat_seg_t *segs, int n, uint32_t win_base,
                          int pages, uint8_t *attr)
{
    for (int i = 0; i < pages; i++)
        attr[i] = 0;

    for (int i = 0; i < n; i++) {
        uint32_t b = segs[i].base;
        uint32_t t = segs[i].top;
        uint8_t bit = (segs[i].attr == PLAT_SEG_CODE) ? PMP_PAGE_CODE
                                                      : PMP_PAGE_DATA;

        for (uint32_t p = b & ~(PMP_PAGE - 1u); p < t; p += PMP_PAGE) {
            int idx = (int)((p - win_base) / PMP_PAGE);

            if (idx >= 0 && idx < pages)
                attr[idx] |= bit;
        }
    }

    return pages;
}

/* Program [win_base, win_end) from the segment list.  Runs of pages with the
 * same attribute become runs of NAPOT blocks; pages that are in no segment
 * are skipped, which is how the hole survives. */
static int pmp_layout(const plat_seg_t *segs, int n, uint32_t win_base,
                      uint32_t win_end, uint32_t *addr, uint32_t *cfg)
{
    uint8_t page[PMP_MAX_PAGES];
    int pages = (int)((win_end - win_base + PMP_PAGE - 1u) / PMP_PAGE);
    int next = 0;

    if (pages > PMP_MAX_PAGES)
        return -1;                      /* a window this wide is a bug */

    pmp_page_attrs(segs, n, win_base, pages, page);

    for (int i = 0; i < pages; ) {
        uint32_t perm, top;
        int j;

        if (!page[i]) {                 /* hole: never covered */
            i++;
            continue;
        }
        perm = (page[i] & PMP_PAGE_DATA) ? (PMP_R | PMP_W | PMP_X)
                                         : (PMP_R | PMP_X);
        for (j = i; j < pages && page[j] == page[i]; j++)
            ;
        /* A block may not spill past the window: a page that is only partly
         * inside it stops the run.  On this board stack_top is page aligned,
         * so this is a guard against a future layout, not a live case. */
        top = win_base + (uint32_t)j * PMP_PAGE;
        if (top > win_end)
            top = win_end;
        next = pmp_cover(win_base + (uint32_t)i * PMP_PAGE, top, perm,
                         addr, cfg, next);
        if (next < 0)
            return -1;
        i = j;
    }
    return next;
}

int plat_board_task_protect(const plat_seg_t *segs, int n)
{
    uint32_t addr[PMP_ENTRIES], cfg[PMP_ENTRIES];
    uint32_t win_base, win_end;
    int i, next;

    if (n <= 0)
        return 1;                       /* nothing to protect: leave PMP alone */

    win_base = segs[0].base & ~(PMP_PAGE - 1u);
    win_end = segs[n - 1].top;

    for (i = 0; i < PMP_ENTRIES; i++) {
        addr[i] = 0;
        cfg[i] = 0;
    }
    next = pmp_layout(segs, n, win_base, win_end, addr, cfg);
    if (next < 0)
        early_puts("\r\n[pmp] window needs more NAPOT blocks than there are "
                   "regions\r\n");

    /* Entry 5 is the trap vector: U-mode has to be able to fetch the first
     * vector slot when a trap arrives from user code. */
    addr[PMP_VEC_ENTRY] = napot((uint32_t)(uintptr_t)trap_vectors, 256u);
    cfg[PMP_VEC_ENTRY] = PMP_NAPOT | PMP_R | PMP_X;

    pmp_program(addr, cfg);
    return 1;
}

/* Program the eight dynamic regions in two phases: all A fields OFF first,
 * then every address, then the real config bytes.
 *
 * pmpaddr's low two bits are WARL on this core: a 4 KiB NAPOT value written
 * while A is OFF reads back in full (0x080071ff), and reads back trimmed
 * (0x080071fc) once A is NAPOT.  That is the G=2 granule doing its job - the
 * low bits are treated as ones when matching - not a failed write.  Writing
 * the addresses while A is OFF still avoids a window in which a stale
 * address is paired with a live A field.  M-mode ignores PMP (no entry sets
 * L), so the brief all-OFF state cannot hurt the kernel. */
static void pmp_program(const uint32_t *addr, const uint32_t *cfg)
{
    uint32_t w[2];

    __asm__ volatile("csrw pmpcfg0, zero");
    __asm__ volatile("csrw pmpcfg1, zero");

    __asm__ volatile("csrw pmpaddr0, %0" : : "r"(addr[0]));
    __asm__ volatile("csrw pmpaddr1, %0" : : "r"(addr[1]));
    __asm__ volatile("csrw pmpaddr2, %0" : : "r"(addr[2]));
    __asm__ volatile("csrw pmpaddr3, %0" : : "r"(addr[3]));
    __asm__ volatile("csrw pmpaddr4, %0" : : "r"(addr[4]));
    __asm__ volatile("csrw pmpaddr5, %0" : : "r"(addr[5]));
    __asm__ volatile("csrw pmpaddr6, %0" : : "r"(addr[6]));
    __asm__ volatile("csrw pmpaddr7, %0" : : "r"(addr[7]));

    for (int g = 0; g < 2; g++)
        w[g] = cfg[4 * g] | (cfg[4 * g + 1] << 8) |
               (cfg[4 * g + 2] << 16) | (cfg[4 * g + 3] << 24);
    __asm__ volatile("csrw pmpcfg0, %0" : : "r"(w[0]));
    __asm__ volatile("csrw pmpcfg1, %0" : : "r"(w[1]));
}

/* A window that grows gets one extra stack-sized step of room, because the
 * stack itself is the thing that usually grows.
 *
 * Page alignment is a hard requirement here after all: the regions this board
 * can program are page-granular (see pmp_layout - sub-page NAPOT blocks are
 * exactly what broke the first attempt at 4 KiB windows), and an unaligned
 * stack_top would leave the stack region unrepresentable, i.e. the task would
 * start with its own stack denied.  Asking the kernel to keep stack_top
 * aligned is cheaper than silently rounding the region out into the guard
 * hole, which is the one thing the guard exists to catch. */
uint32_t plat_window_grow_reserve(void)
{
    return PLAT_STACK_BYTES;
}

int plat_window_needs_stack_alignment(void)
{
    return 1;
}

/* ---- bringing core 1 up ------------------------------------------------ */

/* Core 1 does not run our reset vector: the RP2350 boot ROM holds it in a
 * wait-for-FIFO loop until core 0 pushes the launch sequence
 *
 *     0  0  1  <mtvec>  <sp>  <_start>
 *
 * and echoes every word back.  This is pico-sdk's
 * multicore_launch_core1_raw() protocol for RISC-V.  _start re-reads
 * mhartid, takes .Lsecondary, sees cpu_release and joins the scheduler on
 * its own kernel stack and trap table.  The vector passed here is
 * overwritten there, but the ROM wants a value, so hand it the same
 * direct-mode base core 0 uses. */

#define SIO_FIFO_ST  (SIO_BASE + 0x050u)
#define SIO_FIFO_WR  (SIO_BASE + 0x054u)
#define SIO_FIFO_RD  (SIO_BASE + 0x058u)
#define SIO_FIFO_VLD (1u << 0)
#define SIO_FIFO_RDY (1u << 1)

extern void _start(void);
extern char trap_vectors[];
extern char __kernel_stack_top[];

static void sio_fifo_push(uint32_t v)
{
    for (uint32_t spins = 0; spins < 100000u; spins++) {
        if (mmio_read32(SIO_FIFO_ST) & SIO_FIFO_RDY)
            break;
    }
    mmio_write32(SIO_FIFO_WR, v);

    /* h3.unblock: the RISC-V equivalent of ARM's SEV.  Core 1 waits in
     * h3.block (msleep SLEEPONBLOCK|POWERDOWN); a plain fence would not
     * wake it.  "slt x0, x0, x1" is the custom encoding and is an
     * architectural no-op on a core without the XH3POWER extension. */
    __asm__ volatile("slt x0, x0, x1" ::: "memory");
}

static uint32_t sio_fifo_pop(void)
{
    for (uint32_t spins = 0; spins < 100000u; spins++) {
        if (mmio_read32(SIO_FIFO_ST) & SIO_FIFO_VLD)
            return mmio_read32(SIO_FIFO_RD);
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
    int restarts = 0;

    if (id != 1)
        return 0;               /* this chip has exactly two cores */

    tail[0] = (uint32_t)(uintptr_t)trap_vectors;
    tail[1] = (uint32_t)(uintptr_t)__kernel_stack_top;
    tail[2] = (uint32_t)(uintptr_t)_start;

    for (;;) {
        uint32_t cmd = (seq < 3) ? lead[seq] : tail[seq - 3];

        if (cmd == 0) {
            /* Drain first: a stale word from an earlier attempt would be
             * echoed as the "response" to this one. */
            sio_fifo_drain();
            __asm__ volatile("fence" ::: "memory");
        }
        sio_fifo_push(cmd);

        if (sio_fifo_pop() != cmd) {
            /* Out of step.  Restart the sequence; only a core that never
             * answers at all is a failure. */
            seq = 0;
            if (++restarts > 4)
                return 0;
            continue;
        }
        if (++seq == 6)
            return 1;           /* core 1 is in _start -> .Lsecondary */
    }
}

/* ---- boot -------------------------------------------------------------- */

/* How far the *previous* boot got before it stopped.  Scratch 3 survives a
 * watchdog reset, and it has to be read at entry and kept in RAM: every step
 * below overwrites the same scratch, so by the time the console is up the
 * marker would describe this boot instead of the one that died.
 * Found with it: roughly one reboot in five stalls before the first byte, and
 * the board came back 33.6 s later - 2 x the 16.78 s watchdog period, i.e. two
 * consecutive wedges. */
static uint32_t g_prev_boot_phase;
#if TOS_DIAG
/* Diag-only on purpose: these writes are harmless but they perturb the very
 * boot they measure (the stall below moved from ~1 boot in 4 to 8 of 8 when
 * six of them were added), so a release image keeps the old timing. */
#define BOOT_PHASE(n) mmio_write32(WATCHDOG_SCRATCH(3), (n))
#else
#define BOOT_PHASE(n) do { } while (0)
#endif

void plat_board_early(void)
{
    uint32_t khz;

#if TOS_DIAG
    g_prev_boot_phase = mmio_read32(WATCHDOG_SCRATCH(3));
#endif
    BOOT_PHASE(1u);                 /* entered, before clocks_init          */

    /* clk_peri is OFF after reset, and UART0, IO_BANK0 and PADS_BANK0 all
     * sit behind it.  Touching one of those with the clock gated stalls the
     * bus forever, so the clock tree is set up before anything else.
     *
     * clocks_init() leaves clk_sys on clk_ref (12 MHz) on purpose: only
     * after that is it safe to reset PLL_SYS.  The [A] marker is printed
     * before that reset, so a hang in the PLL step is visible on the wire
     * instead of looking like a dead board. */
    g_clk_hz = 12000000u;

    clocks_init();
    BOOT_PHASE(2u);

    /* The [A] line is divided for the clock the core is actually on now: if
     * the crystal did not start, clk_ref is still the ROM's ROSC (~6.5 MHz)
     * and a 12 MHz divisor would garble the very line that reports it. */
    g_clk_hz = (mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & (1u << 2))
               ? 12000000u : 6500000u;
    wdt_enable();               /* a wedge later resets the board by itself */
    BOOT_PHASE(3u);

    /* TIMER0 is the 64-bit microsecond counter behind TIMERAWL *and* behind
     * the RV32 mtime/mtimecmp pair, and it comes out of reset held in reset.
     * The ROM only deasserts it as part of its flash boot, so after a plain
     * RUN reset it stays frozen: every delay in the panel driver spins its
     * safety bound, mtime never advances, the scheduler never ticks and the
     * console ring is never drained - the boot then hangs after the banner. */
    unreset(RESET_IO_BANK0 | RESET_PADS_BANK0 | RESET_UART0 |
            RESET_TIMER0 | RESET_TIMER1);
    BOOT_PHASE(4u);

    /* Only now is the tick generator writable and its clock final. */
    ticks_init();
    BOOT_PHASE(5u);

    led_init();
    BOOT_PHASE(6u);

    uart_init();
    BOOT_PHASE(7u);
    early_puts("\r\n[A] pre-pll ok sys=0x");
    early_put_hex(g_boot_sys_sel);
    early_puts(" ref=0x");
    early_put_hex(g_boot_ref_sel);
    early_puts(" xosc=0x");
    early_put_hex(g_boot_xosc);
    early_puts("\r\n");
    BOOT_PHASE(8u);                 /* first console line is out            */
    /* Let the shift register finish before clk_peri changes: the last bytes
     * of that line would otherwise be clocked out at 150 MHz with the
     * 12 MHz divisor and arrive as garbage. */
    plat_console_flush();
    BOOT_PHASE(9u);

    pll_sys_init();
    BOOT_PHASE(10u);

    khz = measure_clk_khz();
    BOOT_PHASE(11u);
    if (khz >= 1000u && khz <= 250000u)
        g_clk_hz = khz * 1000u;
    uart_init();                    /* re-divide for the real clk_sys */

    early_puts("[B] clk=0x");
    early_put_hex(g_clk_hz);
    early_puts(" Hz\r\n");
    BOOT_PHASE(12u);
    /* Snapshot the two registers that decide whether this boot can print at
     * all, in the same surviving scratch: a boot that reaches here but puts
     * nothing on the wire is either unclocked (CLK_PERI_CTRL) or has its UART
     * switched off (CR). */
#if TOS_DIAG
    mmio_write32(WATCHDOG_SCRATCH(4), mmio_read32(CLOCKS_BASE + CLK_PERI_CTRL_OFFSET));
    mmio_write32(WATCHDOG_SCRATCH(5), mmio_read32(RP_UART_CR));
#endif
#if TOS_DIAG
    diag_prev_report();             /* what the last boot died doing */
    {
        early_puts("[SCR] ");
        for (uint32_t i = 0; i < 4u; i++) {
            early_put_hex(mmio_read32(WATCHDOG_SCRATCH(i)));
            early_puts(" ");
        }
        early_puts(" prev_phase=");
        early_put_hex(g_prev_boot_phase);
        early_puts("\r\n");
        early_puts("[SCR2] peri=");
        early_put_hex(mmio_read32(WATCHDOG_SCRATCH(4)));
        early_puts(" uart_cr=");
        early_put_hex(mmio_read32(WATCHDOG_SCRATCH(5)));
        early_puts(" reboot=");
        early_put_hex(mmio_read32(WATCHDOG_SCRATCH(7)));
        early_puts("\r\n");
    }
#endif

    /* USB CDC shares the console with UART0.  The wait inside is bounded
     * (~1.5 s scaled by the measured clock), so a board with no USB cable
     * still boots on UART; a cable gets a native COM port as well. */
    usb_cdc_console_init(g_clk_hz);
    g_usb_ready = usb_cdc_hw_ready() ? 1 : 0;
    BOOT_PHASE(14u);

    /* Open the external-interrupt gate (XH3IRQ line 14) now that the
     * controller is up.  Empty when built for polling.  mstatus.MIE is still
     * closed here, so nothing can fire until the scheduler starts. */
    usb_cdc_irq_enable();
    BOOT_PHASE(15u);

    early_puts("[U] usb hw=");
    early_puts(g_usb_ready ? "1" : "0");
    early_puts(" cfg=");
    early_puts(usb_cdc_configured() ? "1" : "0");
    early_puts(" stage=");
    plat_console_putc((char)('0' + usb_cdc_init_stage()));
    early_puts("\r\n");

    early_puts("TinyOS/RP2350  rv32  uart0=115200  usb-cdc=");
    early_puts(g_usb_ready ? "up" : "down");
    early_puts("\r\n");
    BOOT_PHASE(16u);
}

void plat_board_init(void)
{
    /* Resolve the ROM flash helpers first: the JEDEC RDID probe below
     * needs g_rom_connect / g_rom_exit_xip to drop XIP and talk to the
     * part directly. */
    g_rom_connect   = (rv_rom_void_t)rp_rom_func(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    g_rom_exit_xip  = (rv_rom_void_t)rp_rom_func(ROM_FUNC_FLASH_EXIT_XIP);
    g_rom_enter_xip = (rv_rom_void_t)rp_rom_func(ROM_FUNC_FLASH_ENTER_CMD_XIP);
    g_rom_flush     = (rv_rom_void_t)rp_rom_func(ROM_FUNC_FLASH_FLUSH_CACHE);
    g_rom_erase     = (rv_rom_erase_t)rp_rom_func(ROM_FUNC_FLASH_RANGE_ERASE);
    g_rom_program   = (rv_rom_program_t)rp_rom_func(ROM_FUNC_FLASH_RANGE_PROGRAM);
    g_rom_ready = (g_rom_connect && g_rom_exit_xip && g_rom_enter_xip &&
                   g_rom_flush && g_rom_erase && g_rom_program) ? 1 : 0;

    /* Probe the CS0 flash capacity (JEDEC RDID through QMI direct mode,
     * ROM FLASH_DEVINFO as fallback) before anything else, so /proc/flash
     * and the filesystem size show the true part size instead of the
     * configured upper bound or the ROM's conservative 16 MiB default.
     * Nothing rewrites the bootram copy afterwards. */
    g_flash_detected = rp2350_flash_detect();
    BOOT_PHASE(17u);

    /* The USB controller is polled, not interrupt driven, and its state
     * machine must not run in interrupt context - hook it into the
     * scheduler's idle loop instead (see usb_idle_poll above). */
    g_idle_poll = usb_idle_poll;
}

/* ---- power ------------------------------------------------------------- */

void plat_shutdown(void)
{
    console_shutdown();

    /* No power-off on this board; stay in a polling loop so Ctrl-D on UART0
     * still reaches BOOTSEL after the kernel has stopped. */
    for (;;) {
        plat_board_panic_poll();
        for (volatile int i = 0; i < 200; i++)
            ;
    }
}

/* Park the other hart for good before the ROM reboots core 0.
 *
 * Without this the reboot only takes core 0: hart 1 keeps executing the old
 * kernel on the same physical addresses while core 0 re-enters the bootrom,
 * and the board ends up with a CDC port that enumerates and never answers.
 * Measured on rpi-pico2-rv32-smp: the first "reboot" came back, the second
 * left nothing at all (0/15 of a 15-cycle run, against 15/15 for the ARM
 * image with the equivalent fix in plat_reboot).
 *
 * The handshake is the flash-park one, with g_hart1_stopped making the park
 * permanent: rp_flash_park_loop() ends in RP_PARK_STOP_FOREVER(), so the hart
 * stays in RAM feeding the watchdog instead of returning to the kernel. */
static void rv_stop_other_hart(void)
{
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    uint32_t myreq;

    if (cpu_count <= 1)
        return;

    g_hart1_stopped = 1;
    myreq = rp_park_begin(&park, plat_cpu_id());
    RP_PARK_WAKE();
    for (uint32_t i = 0; i < plat_pre_tick_spins(20000000u); i++) {
        if (rp_park_acked(&park, myreq))
            break;
        wdt_feed();
    }
#else
    (void)0;
#endif
}

void plat_reboot(void)
{
    console_shutdown();
    rv_stop_other_hart();
    mmio_write32(WATCHDOG_SCRATCH(7), 0x52424f4fu);   /* "RBOO": asked the ROM */
    rom_reboot_normal();
    /* Reached only when the ROM call returned instead of taking the chip
     * down.  Recorded because it is otherwise invisible: console_shutdown()
     * has already stopped all output, so a failed reboot looks exactly like a
     * board that wedged on its own.  Scratch survives the watchdog reset that
     * eventually rescues this. */
    mmio_write32(WATCHDOG_SCRATCH(7), 0x4641494cu);   /* "FAIL" */
    plat_panic_halt();
}

uint32_t plat_board_cpu_id(void)
{
    return mmio_read32(SIO_CPUID);
}

/* /proc/cpuinfo: the core the image was built for and the measured
 * clk_sys (150 MHz after the PLL step). */
const char *plat_cpu_model(void)
{
    return "Hazard3 (RP2350)";
}

uint32_t plat_cpu_hz(void)
{
    return g_clk_hz;
}

#!/usr/bin/env python3
"""Build a minimal RP2350 boot probe.

Purpose: the full TinyOS UF2 produces no UART and no USB on the bench board.
Everything about the image *format* (PICOBIN block loop, IMAGE_TYPE,
UF2 family IDs, E10 absolute block) has been checked byte-for-byte against
the official MicroPython Pico 2 UF2 and matches.  So the question left is:

    does the RP2350 ROM actually jump into our image, or does TinyOS's
    early C code hang before the first byte reaches the wire?

This script builds a ~2 KiB image with the SAME vector table / IMAGE_DEF /
UF2 packaging as the kernel, but with a tiny freestanding C entry point that:

  * enables clk_peri and releases UART0 / IO_BANK0 / PADS_BANK0 reset,
  * blinks the user LED on GPIO3 (active polarity independent: toggles),
  * emits "PROBE ..." on UART0 (GPIO0/1 = the onboard CH343) at four
    different baud divisors, so whichever clk_sys the ROM handed over at
    is covered (12 / 48 / 125 / 150 MHz),
  * never reads a clk_peri-gated register in an unbounded loop.

If the probe prints but TinyOS does not -> the bug is in the kernel's early
init.  If the probe is silent too -> the ROM is not booting our image at all
(or the board is not powered / not reset), and the image format is suspect.

Usage:
    python scripts/rp2350/boot_probe.py            # build build/rp2350-probe.uf2
    python scripts/rp2350/boot_probe.py --flash   # also wait for the RP2350
                                                # BOOTSEL drive and copy it

Enter BOOTSEL on the ATK-DNRP2350AM by holding BOOT, tapping RST and
releasing both, with the native "USB SLAVE" Type-C cable plugged in.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WORK = os.path.join(ROOT, ".tmp", "rp2350_probe")
OUT = os.path.join(ROOT, "build", "rp2350-probe.uf2")

FLASH_BASE = 0x10000000
UF2_FAMILY = 0xE48BFF59       # RP2350 ARM Secure
UF2_ABS_FAMILY = 0xE48BFF57    # RP2350 ABSOLUTE / E10 workaround
UF2_ABS_ADDR = 0x10FFFF00
UF2_IGNORE_BLOCK = 0x9957E304
UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30

START_S = r"""
    .syntax unified
    .thumb

    .section .vectors, "ax"
    .align 8
    .globl __vectors
__vectors:
    .word 0x20082000
    /* lld already sets bit 0 of a Thumb STT_FUNC symbol, so adding 1 here
     * would produce an even (non-Thumb) reset vector. */
    .word _start
    .rept 14
    .word hang
    .endr

    .section .picobin_block, "a"
    .align 2
    .globl __image_def_start
__image_def_start:
    .word 0xffffded3
    .byte 0x42, 0x01
    .hword 0x1021
    .byte 0xff
    .hword 0x0001
    .byte 0x00
    .word 0                    /* patched to the end-block offset */
    .word 0xab123579

    .section .text, "ax"
    .globl hang
    .thumb_func
hang:
    b hang
"""

PROBE_C = r"""
#include <stdint.h>

#define WR(a, v) (*(volatile uint32_t *)(uintptr_t)(a) = (uint32_t)(v))
#define RD(a)    (*(volatile uint32_t *)(uintptr_t)(a))

#define CLOCKS  0x40010000u
#define RESETS  0x40020000u
#define IO      0x40028000u
#define PADS    0x40038000u
#define UART0   0x40070000u
#define SIO     0xd0000000u

#define LED_BIT (1u << 3)

static void delay(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        __asm__ volatile ("nop");
}

/* No FR reads: a clock-gated peripheral would stall the AHB fabric and a
 * software timeout cannot help.  Wait a fixed number of cycles instead. */
static void uart_baud(uint32_t ibrd, uint32_t fbrd)
{
    WR(UART0 + 0x30, 0u);              /* CR: UARTEN off                */
    WR(UART0 + 0x24, ibrd);
    WR(UART0 + 0x28, fbrd);
    WR(UART0 + 0x2c, 0x70u);          /* 8N1, FIFO enable             */
    WR(UART0 + 0x30, 0x301u);         /* UARTEN | TXE | RXE            */
}

static void puts_raw(const char *s)
{
    while (*s) {
        WR(UART0 + 0x00u, (uint8_t)*s++);
        delay(4000u);
    }
}

/* Re-enter the ROM's BOOTSEL drive without touching the board.  RP2350 ARM
 * Secure: the (code, flags) lookup pointer is a 16-bit value at 0x16,
 * ROM_FUNC_REBOOT = ROM_TABLE_CODE('R','B') = 0x4252, and the reboot type
 * is REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL (2) | NO_RETURN_ON_SUCCESS (0x100). */
typedef void *(*rom_lookup_t)(uint32_t, uint32_t);
typedef int (*rom_reboot_t)(uint32_t, uint32_t, uint32_t, uint32_t);

static void reboot_to_bootsel(void)
{
    rom_lookup_t lookup = (rom_lookup_t)(uintptr_t)(*(volatile uint16_t *)0x16u);
    rom_reboot_t reboot = (rom_reboot_t)
        ((uintptr_t)lookup(0x4252u, 0x0004u) | 1u);
    reboot(0x102u, 10u, 0u, 0u);
}

void _start(void)
{
    uint32_t bits = (1u << 26) | (1u << 9) | (1u << 6);  /* UART0/PADS/IO */

    /* clk_peri = clk_sys / 1, enabled.  CLOCKS and RESETS are never gated. */
    WR(CLOCKS + 0x4cu, 0x00010000u);  /* CLK_PERI_DIV = 1.0             */
    WR(CLOCKS + 0x48u, 0x00000800u);  /* CLK_PERI_CTRL.ENABLE           */
    delay(2000u);

    WR(RESETS + 0x00u, RD(RESETS + 0x00u) & ~bits);
    for (uint32_t i = 0; i < 200000u; i++) {
        if ((RD(RESETS + 0x08u) & bits) == bits)
            break;
    }

    /* LED on GPIO3 as SIO.  Toggle it regardless of active polarity.
     * 0x180 = PAD_OD (bit 7) | PAD_ISO (bit 8).  RP2350 leaves ISO set at
     * reset, so a pad configured without clearing it stays isolated. */
    WR(PADS + 0x10u, (RD(PADS + 0x10u) | 0x40u) & ~0x180u);
    WR(IO + 0x1cu, 5u);               /* GPIO3 FUNCSEL = SIO           */
    WR(SIO + 0x38u, LED_BIT);          /* OE_SET                        */
    WR(SIO + 0x18u, LED_BIT);          /* OUT_SET                       */
    WR(SIO + 0x20u, LED_BIT);          /* OUT_CLR                       */

    /* UART0 TX = GPIO0, RX = GPIO1; this is exactly how the onboard
     * CH343 is wired on the ATK-DNRP2350AM (ALIENTEK wiki, UART example). */
    WR(PADS + 0x04u, (RD(PADS + 0x04u) | 0x40u) & ~0x180u);
    WR(PADS + 0x08u, (RD(PADS + 0x08u) | 0x40u) & ~0x180u);
    WR(IO + 0x04u, 2u);               /* GPIO0 FUNCSEL = UART0 TX      */
    WR(IO + 0x0cu, 2u);               /* GPIO1 FUNCSEL = UART0 RX      */
    WR(UART0 + 0x44u, 0x7ffu);        /* ICR: clear all interrupts     */

    /* Divisors for 115200 baud at four plausible clk_sys rates.  One of
     * these must be readable; the others just add harmless line noise. */
    const uint32_t ibrd[4] = { 6u, 26u, 67u, 81u };
    const uint32_t fbrd[4] = { 33u, 3u, 52u, 24u };

    uint32_t cycles = 0;
    for (;;) {
        for (int m = 0; m < 4; m++) {
            uart_baud(ibrd[m], fbrd[m]);
            puts_raw("\r\nPROBE clk12/48/125/150 mode=");
            WR(UART0 + 0x00u, (uint8_t)('0' + m));
            delay(4000u);
            puts_raw(" alive\r\n");
        }
        /* Alternating SET/CLR gives a visible heartbeat regardless of the
         * LED's active polarity. */
        WR(SIO + 0x18u, LED_BIT);
        delay(200000u);
        WR(SIO + 0x20u, LED_BIT);
        delay(200000u);

        /* After a while, hand control back to the ROM bootloader so the next
         * firmware can be dropped on the drive without pressing BOOT/RST. */
        if (++cycles >= 100u)
            reboot_to_bootsel();
    }
}
"""

LINKER = r"""
MEMORY {
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 1M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}
ENTRY(_start)
SECTIONS {
    .text : {
        KEEP(*(.vectors))
        KEEP(*(.picobin_block))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH
    .data : { *(.data*) . = ALIGN(4); } > RAM AT > FLASH
    .bss (NOLOAD) : { *(.bss*) *(COMMON) . = ALIGN(8); } > RAM
    /DISCARD/ : { *(.comment) *(.ARM.exidx*) *(.ARM.attributes) *(.note*) *(.eh_frame*) }
}
"""


def run(cmd):
    print("  " + " ".join(cmd))
    subprocess.run(cmd, check=True)


def elf_to_flat(path):
    """Copy PT_LOAD file bytes at their physical (LMA) addresses."""
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF"
    e_phoff = struct.unpack_from("<I", d, 28)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", d, 42)
    top = 0
    segs = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from(
            "<IIIII", d, o)
        if p_type != 1 or p_filesz == 0:      # PT_LOAD
            continue
        if not (FLASH_BASE <= p_paddr < FLASH_BASE + 0x1000000):
            continue
        off = p_paddr - FLASH_BASE
        segs.append((off, d[p_offset:p_offset + p_filesz]))
        top = max(top, off + p_filesz)
    flat = bytearray(top)
    for off, blob in segs:
        flat[off:off + len(blob)] = blob
    return flat


def uf2_block(addr, payload, family, flags, num, total):
    b = bytearray(512)
    # UF2 blocks are always 256 bytes of payload; the remaining 220 bytes
    # are the extension area.  The RP2350 ROM rejects short blocks.
    payload = bytes(payload).ljust(256, b"\x00")[:256]
    struct.pack_into("<IIIIIIII", b, 0,
                      UF2_MAGIC0, UF2_MAGIC1, flags, addr,
                      256, num, total, family)
    b[32:32 + len(payload)] = payload
    struct.pack_into("<I", b, 508, UF2_MAGIC_END)
    return bytes(b)


def make_uf2(flat):
    # E10 absolute block, byte-identical to the official MicroPython image.
    abs_blk = bytearray(512)
    struct.pack_into("<IIIIIIII", abs_blk, 0,
                     UF2_MAGIC0, UF2_MAGIC1, 0x0000A000, UF2_ABS_ADDR,
                     256, 0, 2, UF2_ABS_FAMILY)
    for i in range(32, 32 + 256):
        abs_blk[i] = 0xEF
    struct.pack_into("<I", abs_blk, 32 + 256, UF2_IGNORE_BLOCK)
    struct.pack_into("<I", abs_blk, 508, UF2_MAGIC_END)

    marker = flat.find(b"\xd3\xde\xff\xff")
    if marker < 0 or marker > 4096:
        raise SystemExit("no PICOBIN IMAGE_DEF in the first 4 KiB")
    end_off = (len(flat) + 15) & ~15
    flat = bytearray(flat)
    flat.extend(b"\x00" * (end_off + 20 - len(flat)))
    struct.pack_into("<I", flat, marker + 12, end_off - marker)
    struct.pack_into("<IIIII", flat, end_off,
                     0xFFFFDED3, 0x000001FE, 0x000001FF,
                     (marker - end_off) & 0xFFFFFFFF, 0xAB123579)

    # Every UF2 block in the official images carries a full 256-byte payload.
    # A short final block makes the RP2350 ROM silently discard the whole
    # download (the .uf2 file just stays on the drive).  Pad to a whole
    # number of blocks, with a 4 KiB floor so the file also clears any
    # minimum-download-size behaviour in the ROM.
    if len(flat) < 4096:
        flat.extend(b"\x00" * (4096 - len(flat)))
    flat.extend(b"\x00" * ((-len(flat)) % 256))

    sp, pc = struct.unpack_from("<II", flat, 0)
    print(f"  MSP=0x{sp:08x}  reset=0x{pc:08x}  "
          f"IMAGE_DEF@0x{marker:x}  end block@0x{end_off:x}")
    if pc & 1 == 0 or not (FLASH_BASE <= pc < FLASH_BASE + 0x1000000):
        raise SystemExit("bad reset vector")

    n = (len(flat) + 255) // 256
    out = [bytes(abs_blk)]
    for i in range(n):
        out.append(uf2_block(FLASH_BASE + i * 256,
                             bytes(flat[i * 256:(i + 1) * 256]),
                             UF2_FAMILY, 0x2000, i, n))
    return b"".join(out)


def build():
    os.makedirs(WORK, exist_ok=True)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    paths = {}
    for name, text in (("start.S", START_S), ("probe.c", PROBE_C),
                       ("probe.ld", LINKER)):
        p = os.path.join(WORK, name)
        open(p, "w", newline="\n").write(text)
        paths[name] = p

    clang = shutil.which("clang")
    lld = shutil.which("ld.lld") or shutil.which("lld")
    if not clang or not lld:
        raise SystemExit("need clang and ld.lld on PATH (MSYS2 UCRT64)")
    base = ["--target=thumbv8m.main-none-eabi", "-mcpu=cortex-m33",
            "-mthumb", "-mfloat-abi=soft", "-ffreestanding", "-fno-builtin",
            "-fno-pic", "-fno-pie", "-fno-stack-protector", "-O2",
            "-fno-asynchronous-unwind-tables", "-fno-unwind-tables"]
    o1 = os.path.join(WORK, "start.o")
    o2 = os.path.join(WORK, "probe.o")
    elf = os.path.join(WORK, "probe.elf")
    run([clang] + base + ["-c", paths["start.S"], "-o", o1])
    run([clang] + base + ["-c", paths["probe.c"], "-o", o2])
    run([lld, "-m", "armelf", "-T", paths["probe.ld"], "-nostdlib",
         "--gc-sections", "-s", o1, o2, "-o", elf])
    flat = elf_to_flat(elf)
    uf2 = make_uf2(flat)
    open(OUT, "wb").write(uf2)
    print(f"  wrote {OUT} ({len(uf2)} bytes, {len(uf2)//512} blocks)")


def bootdrive():
    for letter in "DEFGHIJKLMNOPQRSTUVWXYZ":
        root = f"{letter}:\\"
        if not os.path.exists(root):
            continue
        try:
            if os.path.exists(os.path.join(root, "INFO_UF2.TXT")):
                return root
        except OSError:
            pass
    return None


def flash(timeout):
    import serial
    tail = None
    try:
        tail = serial.Serial("COM14", 115200, timeout=0.05)
        tail.setDTR(False)
        tail.setRTS(False)
    except Exception as e:
        print(f"  (COM14 tail unavailable: {e})")
    print("  waiting for the RP2350 BOOTSEL drive ...", flush=True)
    deadline = time.time() + timeout
    drive = None
    while time.time() < deadline:
        drive = bootdrive()
        if drive:
            break
        if tail:
            tail.read(4096)
        time.sleep(0.2)
    if not drive:
        print("  timed out: hold BOOT, tap RST, release; "
              "make sure the native USB SLAVE cable is plugged in")
        return False
    print(f"  found {drive}, copying probe ...", flush=True)
    with open(OUT, "rb") as f, open(os.path.join(drive, "probe.uf2"), "wb") as g:
        shutil.copyfileobj(f, g)
    print("  copied; watching COM14 for 12 s ...", flush=True)
    end = time.time() + 12
    buf = bytearray()
    while time.time() < end:
        if tail:
            buf += tail.read(4096)
        time.sleep(0.05)
    if tail:
        tail.close()
    print(f"  COM14: {len(buf)} bytes: {bytes(buf[:600])!r}")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flash", action="store_true")
    ap.add_argument("--timeout", type=float, default=120)
    a = ap.parse_args()
    build()
    if a.flash:
        flash(a.timeout)


if __name__ == "__main__":
    main()

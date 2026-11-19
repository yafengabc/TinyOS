commit: gate run at ae311bd + the fixes below (plat_reboot parks the other core)
baseline: ae311bd (unique USB serial 689c07f, usb_delay_ms f1be069)
change: repeat the power-on path on the two Pico 2 SMP images; bounds for #31/#29
target/config: rpi-pico2-smp, rpi-pico2-rv32-smp (Pico 2, W25Q64)
board/port: Raspberry Pi Pico 2, native CDC (2e8a:000a, now SER=62BD11016CFF484E)
            plus UART0 on CH343 (COM19) as the second witness
toolchain: go 1.27.x windows/amd64, clang 23.1.4, python 3.14
host: Windows, Git Bash
T0: go build ./... -> PASS; go test ./... -count=1 -> PASS
T1: go run ./cmd/tos build all -> PASS (18/18)
T2: QEMU smoke -> covered by go test ./cmd/tostest (tostest passed); no shared
    kernel path changed beyond the /proc/usbpoll entry and the weak hooks
    (plat_usb_poll_ns, plat_flash_probe_debug), both defaulting to 0

Results
-------
Gate = "reset, then see whether a shell comes back, with a fresh uptime".
`reboot` is the reset: plat_reboot() goes through the ROM's BOOT_TYPE_NORMAL
entry, which re-scans flash for the IMAGE_DEF and boots the image - the path a
power-on takes.  A cycle only counts when `uptime` reports <= 12 s, so a shell
that was never reset cannot pass by answering the next command.

| image | before the fixes | after | per-cycle uptime |
| --- | --- | --- | --- |
| rpi-pico2-smp | 1/3 | **20/20** | 0.52-0.53 s |
| rpi-pico2-rv32-smp | 1/15 | **35/38** at a 20 s window, **10/10** at 45 s | 0.40-0.45 s |

The three misses on RV32 are not boot failures: the CDC did not enumerate within
the 20 s window, and the next cycle's reset worked on the same board (a wedged
board cannot be reset, so it had booted).  Re-running with a 45 s window gave
10/10, so the misses are enumeration latency on this host, not a dead image.

Logs: build/gate-rpi-pico2-smp-20.csv, build/gate-rpi-pico2-rv32-smp.csv,
build/gate-rpi-pico2-rv32-smp-2.csv, build/gate-rpi-pico2-rv32-smp-45s.csv,
build/gate-rv32-usbpoll.csv

The bug the gate found: reboot did not park the other core
----------------------------------------------------------
`plat_reboot()` on both RP2350 ports called the ROM reboot with the other core
still running the old kernel on the same physical addresses.  `rom_enter_bootsel()`
had always parked it first (`rom_stop_other_core()`), but the reboot path did
not.  Symptom, measured on rpi-pico2-smp before the fix: the first `reboot` came
back, the second left a CDC port that enumerated, opened, and never answered -
the exact shape the AIRCR comment warns about.  One of three cycles survived.

Fix:
  * ARM (`board/rp2350-generic`): `rom_stop_other_core()` before `rom_reboot()`.
    g_core1_stopped keeps the parked core in the loop for good, so there is
    nothing to unpark.
  * RV32 (`board/rp2350rv-generic`): the port had no equivalent.  It now sets
    g_hart1_stopped, raises the flash-park request, waits for the ack (bounded,
    feeding the watchdog) and reboots.  A new RP_PARK_STOP_FOREVER() hook makes
    the parked hart stay in RAM instead of returning to the kernel.
  After the fix: ARM 15/15 then 20/20, RV32 14/15, 13/15 and 8/8 (the misses
  being the enumeration latency above).  Before: 1/3 and 1/15.

Recovery, for the record: a wedged board is not dead.  Ctrl-D on the CDC port
(`scripts/rp2350/bootsel_poke.py COM27 25 --dtr`) still gets it into BOOTSEL,
twice during this run, so the tick and the USB controller were alive and only
the shell was not.  The 1200-baud touch does nothing here - that is pico-sdk's
reset interface, not ours.

Two method notes that changed the outcome
-----------------------------------------
1. DTR must be asserted on the CDC port or it reads as an absent host and drops
   every byte.  With dtr=False the console looked exactly like a dead board;
   dtr=True answered the prompt immediately.
2. A cycle has to prove it is a new boot.  Without the uptime check, a reset
   that never happened is indistinguishable from a successful one - the first
   version of this run passed 15/15 that way, which is why uptime is now a
   gate criterion and not a detail.

#31 RP2040 pump: NOT RUN
------------------------
The RP2040 port is not on the bench this round, so all four measurements are
NOT RUN.  The code is in place; nothing about it was exercised here.

#29 Hazard3: one usb_cdc_poll() costs 2660 ns
--------------------------------------------
New `/proc/usbpoll` (port hook plat_usb_poll_ns(), weak 0 elsewhere) measures a
steady-state poll against mtime over a fixed slice:

    usbpoll: 2666 ns/poll (93773 polls per 250 ms)
    usbpoll: 2666 ns/poll
    usbpoll: 2659 ns/poll

Two consequences worth recording before anyone copies the ARM bound:

  * 250 ms is ~94 000 polls on this port, not the 40 000 the ARM pump uses.
  * the earlier hung boot with a 40 000-poll bound cannot have been a duration
    problem: 40 000 x 2.66 us = 106 ms.  So the ARM shape fails here for a
    reason other than budget - the RV32 loop also keeps a 40-cycle gap between
    polls, and removing it is the more likely culprit.  Do not switch the pump
    to "copy ARM" until that trap (mcause=0x3 at 0x10013d0a, a panic path) is
    explained.

#4 USB serial: done, and visible here
-------------------------------------
`689c07f` gives each RP2350 board its chip id as iSerialNumber; the CDC on this
run enumerates as SER=62BD11016CFF484E, and the port moved from COM17 to COM27
exactly once when the image with the new descriptor was flashed.  RP2040 still
sends "0001" (no SYS_INFO; it needs a 0x4B read over the SSI at USB init) and
is not implemented.

#5 usb_delay_ms: done
---------------------
`f1be069` calibrated the loop to its measured per-iteration cost.

RV32 warm reboot leaves UART0 silent: not RV32-specific
-------------------------------------------------------
OPEN-ITEMS 5b-5 recorded this on RV32.  It reproduced here on the **ARM** image
too: after the BOOTSEL flash that started this run, UART0 was silent for all 20
gate cycles while the CDC answered every time (0/20 UART, 20/20 CDC).  So the
trigger is "the boot that follows a BOOTSEL flash", not the RISC-V port.

Consequence for the gate: UART0 cannot be the primary witness on this board
after a flash, which is a pity because it is the channel that does not
re-enumerate.  The gate therefore drives the reset over CDC and records UART0 as
a separate (currently always-silent) column.

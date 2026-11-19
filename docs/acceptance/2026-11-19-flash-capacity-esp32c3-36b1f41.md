commit: 36b1f41 (code); this card is the follow-up docs commit
change: generic flash-capacity detection; clamp littlefs to the detected part; /proc/flash + flashinfo
baseline: 92c4b43
target/config: esp32c3-generic (board/esp32c3-generic/board.conf), qemu rv32/rv32smp/arm, `tos build all`
board/port/image: ESP32-C3 devkit 4 MiB, COM4, build/tinyos-esp32c3-generic-full.bin
toolchain: go 1.27.3 windows/amd64, clang 23.1.4, qemu 11.1.3, python 3.14.9
host: Windows, PowerShell 7
T0: go build ./... -> PASS; go vet ./... -> PASS; go test ./... -count=1 -> PASS
    (4 skips, all pre-existing: TestLittlefsImageRoundTrip, TestELFToMini,
    TestMiniHeaderFields, TestCorruptDetected; no new skips)
T1: go run ./cmd/tos build all -> PASS (18/18, rv32nc included)
T2: smoke rv32/rv32smp/arm -> PASS; scripts/soak.py --cycles 3 -> 3/3 PASS
T3: ESP32-C3 COM4 -> PASS (boot, flashinfo, df, write/read/rm, 20-cycle soak)
    flashinfo: detected 4194304, configured 4194304, lfs_offset 524288, lfs_blocks 896
    df: littlefs 3670016 total / 307200 used / 8%
    echo hi > a.txt; cat a.txt -> hi; rm a.txt -> prompt, no error
    scripts/esp-c3/soak.py -p COM4 --cycles 20 -> 20 cycles, 0 resets, 0 errors
T4: NOT RUN (not required for this change; no 1000-round dd/rm run)
logs: build/testlogs/go-test.log, build/testlogs/build-all3.log,
      build/testlogs/c3-flashinfo-final.log, build/soak-esp32c3-generic-*.log
      (local only, not committed)

What changed and how it was proved
----------------------------------
1. Detection is generic. `plat_flash_detected_bytes()` is a per-chip override
   with a weak default of 0; `plat_lfs_blocks()` takes
   min(detected ?: configured, configured) minus the partition offset.  No
   board has to change to keep its old behaviour (detected = 0 -> configured).

2. C3 detection reads the boot loader's JEDEC record at
   rom_spiflash_legacy_data (0x3fcdfff0), whose device_id is the ID read at
   boot.  It must be read before esp_rom_spiflash_config_param(), because that
   call stores the deviceId argument (0 here) over the field.  The observed
   ID is 0x00c22016; low byte 0x16 -> 1 << 22 = 4 MiB.  The ROM's chip_size
   field is a fixed 2 MiB default and was not used; the same detected size is
   now passed to the ROM so its own range check matches the part.

3. A shipped image may be smaller than the part, so littlefs must still take
   the superblock size at mount (cfg.block_count = 0).  After mount,
   ramdisk_init() refuses a superblock larger than
   plat_lfs_blocks() before any write, then grows to the effective size.
   Setting cfg.block_count to the effective size directly is wrong: littlefs
   requires equality with the superblock, so the normal 75-block image fails
   to mount (observed as "bin/msh not found" and a panic).

4. `df` was still reporting the build-time PLAT_LFS_BLOCKS, which made an
   8 MiB-configured board look as if it had 7.5 MiB on a 4 MiB part.  It now
   reports plat_lfs_blocks().

Cap tests on the real C3
------------------------
- Normal (flash_bytes = 4m): detected = configured = 4194304,
  lfs_blocks = 896, df total = 3670016.
- Temporary flash_bytes = 8m: detected 4194304, configured 8388608,
  lfs_blocks 896, df total 3670016.  The cap held; the earlier 7864320
  reading was the df reporting bug above.
- Refusal path: a temporary build that ignored detection grew the on-chip
  superblock to 1920 blocks.  A kernel-only update with the 4 MiB config then
  printed
      [ramdisk] image claims 1920 blocks, device has 896; refusing
  before the first write.  The board was reflashed with the full image
  afterwards and booted normally; this proves the fail-closed branch.

qemu-virt geometry
------------------
The embedded filesystem cannot grow.  rv32im binaries (rv32nc) plus the new
flashinfo program overflowed the old 300 KiB disk with "No more free space".
It is now 512 KiB (ramdisk_sectors = 1024, PLAT_LFS_BLOCKS = 128): with a
1 MiB minimum part and the filesystem at 512 KiB, that is the largest image
that can exist on the smallest supported chip.  df on rv32 shows 524288
total / 294912 used.  Flash-backed boards keep the 300 KiB shipped image and
grow on first mount, which already fits a 1 MiB part.

Not run
-------
RP2350/ATK/RP2040 hardware detection is not implemented yet (weak default 0
keeps their current behaviour); T4 and Pico 2 were not run for this change.

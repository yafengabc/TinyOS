package build

import (
	"debug/elf"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// RP2040 image packaging.
//
// The boot ROM loads a 256-byte stage-2 from flash offset 0, verifies a
// CRC-32/MPEG-2 over its first 252 bytes, then jumps to it; the stage-2 jumps
// to the main firmware reset vector at 0x10000100.  The kernel is linked
// there (XIP), and every flash-backed board puts its littlefs image at
// 0x80000 (512 KiB): the largest kernel here is ~192 KiB, so half a MiB
// leaves comfortable headroom and avoids zero-padding a whole 1 MiB into
// the UF2.  Everything is wrapped in a UF2 (family 0xe48bff56) that drops
// onto the RPI-RP2 drive.

const rp2040UF2Family = 0xe48bff56

// rpLfsOffset is the flash offset of the littlefs image for the RP2040 and
// RP2350 UF2 packers (and, by agreement, for the ESP32-C3 esptool command
// and scripts/esp-c3/flash.py).  checkLayout verifies each board's
// PLAT_LFS_LBA agrees with it.
const rpLfsOffset = 0x80000

// RP2350 ARM Secure.  The RISC-V and ARM Non-Secure families are
// 0xe48bff5a and 0xe48bff5b; our image_def.S declares an ARM Secure EXE.
const rp2350UF2Family = 0xe48bff59

// RP2350 RISC-V Secure EXE.  The family id follows the PICOBIN IMAGE_TYPE,
// not the chip: an image the ROM boots on Hazard3 must use this one, or the
// BOOTSEL drive accepts the blocks and the ROM ignores them.
const rp2350RVUF2Family = 0xe48bff5a

// "Absolute" is a pseudo-family: the ROM treats those blocks as commands
// rather than flash payload.  RP2350-E10 (A2 boot ROM) breaks UF2
// drag-and-drop downloads unless the file starts with one such block at
// the top of the 16 MiB XIP window; picotool's gen_abs_block() emits it for
// every flash image.  MicroPython's official Pico 2 UF2 has exactly this
// block, so match it byte for byte.
const rp2350AbsoluteFamily = 0xe48bff57
const rp2350AbsBlockAddr = 0x10ffff00
const uf2ExtensionRP2IgnoreBlock = 0x9957e304

// Both chips map the QSPI flash at 0x10000000, so elfToFlashRaw and
// flatToUF2 are shared by the two ports.
const rp2040FlashBase = 0x10000000

// crc32mpeg2 is CRC-32/MPEG-2: poly 0x04c11db7, no input/output reflection,
// init 0xffffffff, no final xor.  It is the checksum the RP2040 boot ROM
// requires over the first 252 bytes of the stage-2 bootloader.
func crc32mpeg2(data []byte) uint32 {
	crc := uint32(0xffffffff)
	for _, b := range data {
		crc ^= uint32(b) << 24
		for i := 0; i < 8; i++ {
			if crc&0x80000000 != 0 {
				crc = (crc << 1) ^ 0x04c11db7
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

// elfHeadersEnd returns the offset just past the program header table of a
// 32-bit little-endian ELF.  Go's debug/elf keeps the program headers but not
// their file offset, and that offset is what tells us whether a PT_LOAD is
// nothing but the headers themselves.
func elfHeadersEnd(path string) (uint64, error) {
	fh, err := os.Open(path)
	if err != nil {
		return 0, err
	}
	defer fh.Close()

	var hdr [0x34]byte
	if _, err := io.ReadFull(fh, hdr[:]); err != nil {
		return 0, err
	}
	if hdr[0] != 0x7f || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F' {
		return 0, fmt.Errorf("%s is not an ELF file", path)
	}
	phoff := binary.LittleEndian.Uint32(hdr[0x1c:])
	phentsize := binary.LittleEndian.Uint16(hdr[0x2a:])
	phnum := binary.LittleEndian.Uint16(hdr[0x2c:])
	return uint64(phoff) + uint64(phentsize)*uint64(phnum), nil
}

// elfToFlashRaw lays out the load segments of an RP2040 ELF as a raw binary,
// and reports the flash offset its first byte belongs at.
//
// Only segments that actually carry file bytes and whose physical address is
// inside the flash window belong in the image.  The .bss segment is NOLOAD
// (Filesz 0) and lld gives it Paddr == Vaddr in SRAM, so both checks skip it;
// the .data segment has Vaddr in SRAM but its Paddr is the flash LMA picked
// by the linker script's "AT > FLASH", which is what we flash.
//
// Two subtleties, both measured on the rp2040 artifact:
//
//   - lld maps the ELF header and program header table into a PT_LOAD of its
//     own when the first section does not start on a 64 KiB boundary.  This
//     kernel links at 0x10000100, so that segment covers 0x10000000..0x100000F4
//     and holds nothing but metadata.  Flashing it would put "\x7fELF" exactly
//     where the stage-2 jumps - the board then dies before printing anything.
//     Such a segment is identified by offset 0 and a size no larger than the
//     headers themselves, and is dropped.
//   - the remaining segments do not necessarily start at the flash base, so
//     the caller is told where the blob goes (that offset is where the vector
//     table - word 0 = MSP, word 1 = reset - ends up).
func elfToFlashRaw(path string) ([]byte, uint32, error) {
	f, err := elf.Open(path)
	if err != nil {
		return nil, 0, err
	}
	defer f.Close()
	hdrsEnd, err := elfHeadersEnd(path)
	if err != nil {
		return nil, 0, err
	}
	const flashWindow = 0x1000000 // 16 MiB of XIP address space
	inFlash := func(p *elf.Prog) bool {
		return p.Type == elf.PT_LOAD && p.Filesz > 0 &&
			p.Paddr >= rp2040FlashBase && p.Paddr < rp2040FlashBase+flashWindow &&
			!(p.Off == 0 && uint64(p.Filesz) <= hdrsEnd)
	}
	var minOff, maxOff uint64
	first := true
	for _, p := range f.Progs {
		if !inFlash(p) {
			continue
		}
		off := p.Paddr - rp2040FlashBase
		if first || off < minOff {
			minOff = off
			first = false
		}
		if end := off + p.Filesz; end > maxOff {
			maxOff = end
		}
	}
	if maxOff == 0 || maxOff <= minOff {
		return nil, 0, fmt.Errorf("elf %s has no load segments", path)
	}
	raw := make([]byte, maxOff-minOff)
	for _, p := range f.Progs {
		if !inFlash(p) {
			continue
		}
		off := p.Paddr - rp2040FlashBase - minOff
		data, err := io.ReadAll(p.Open())
		if err != nil {
			return nil, 0, err
		}
		copy(raw[off:], data)
	}
	return raw, uint32(minOff), nil
}

// flatToUF2 converts a raw flash image into an RP2040-family UF2 stream.
func flatToUF2(flat []byte, family uint32) []byte {
	const blockPayload = 256
	numBlocks := (len(flat) + blockPayload - 1) / blockPayload
	out := make([]byte, 0, numBlocks*512)
	for i := 0; i < numBlocks; i++ {
		var buf [512]byte
		binary.LittleEndian.PutUint32(buf[0:4], 0x0A324655)  // magic_start0
		binary.LittleEndian.PutUint32(buf[4:8], 0x9E5D5157)  // magic_start1
		binary.LittleEndian.PutUint32(buf[8:12], 0x00002000) // flags: FAMILY_ID_PRESENT
		binary.LittleEndian.PutUint32(buf[12:16], uint32(rp2040FlashBase)+uint32(i*blockPayload))
		payload := blockPayload
		if i == numBlocks-1 {
			payload = len(flat) - i*blockPayload
		}
		binary.LittleEndian.PutUint32(buf[16:20], uint32(payload))
		binary.LittleEndian.PutUint32(buf[20:24], uint32(i))
		binary.LittleEndian.PutUint32(buf[24:28], uint32(numBlocks))
		binary.LittleEndian.PutUint32(buf[28:32], family) // family id in the file_size slot
		copy(buf[32:32+payload], flat[i*blockPayload:i*blockPayload+payload])
		binary.LittleEndian.PutUint32(buf[508:512], 0x0AB16F30) // magic_end
		out = append(out, buf[:]...)
	}
	return out
}

// rp2350AbsBlock builds the RP2350-E10 workaround block.  256 bytes of
// 0xef at 0x10ffff00, an RP2_IGNORE_BLOCK extension tag so the ROM does not
// actually program that flash sector, and num_blocks=2 (the value picotool
// writes; the block stands alone).  It is a separate one-block "file" in
// front of the real firmware UF2 stream, exactly like the official images.
func rp2350AbsBlock() []byte {
	var b [512]byte
	binary.LittleEndian.PutUint32(b[0:4], 0x0A324655)  // magic_start0
	binary.LittleEndian.PutUint32(b[4:8], 0x9E5D5157)  // magic_start1
	binary.LittleEndian.PutUint32(b[8:12], 0x0000a000) // FAMILY_ID | EXTENSION_FLAGS
	binary.LittleEndian.PutUint32(b[12:16], rp2350AbsBlockAddr)
	binary.LittleEndian.PutUint32(b[16:20], 256) // payload_size
	binary.LittleEndian.PutUint32(b[20:24], 0)   // block_no
	binary.LittleEndian.PutUint32(b[24:28], 2)   // num_blocks (picotool quirk)
	binary.LittleEndian.PutUint32(b[28:32], rp2350AbsoluteFamily)
	for i := 32; i < 32+256; i++ {
		b[i] = 0xef
	}
	binary.LittleEndian.PutUint32(b[32+256:36+256], uf2ExtensionRP2IgnoreBlock)
	binary.LittleEndian.PutUint32(b[508:512], 0x0AB16F30) // magic_end
	return b[:]
}

// rp2350WriteEndBlock appends the ignored end block that makes the metadata
// a two-block loop: START, 2BS_IGNORED(1 word), 2BS_LAST(1 word), a negative
// offset back to the IMAGE_DEF, END.  Byte pattern matches
// pico_crt0/embedded_end_block.inc.S and the MicroPython release image.
func rp2350WriteEndBlock(flat []byte, markerOff, endOff uint32) {
	binary.LittleEndian.PutUint32(flat[endOff+0:endOff+4], 0xffffded3)
	binary.LittleEndian.PutUint32(flat[endOff+4:endOff+8], 0x000001fe)
	binary.LittleEndian.PutUint32(flat[endOff+8:endOff+12], 0x000001ff)
	binary.LittleEndian.PutUint32(flat[endOff+12:endOff+16], markerOff-endOff) // wraps to 2's complement
	binary.LittleEndian.PutUint32(flat[endOff+16:endOff+20], 0xab123579)
}

// rp2350CloseBlockLoop turns the single self-referencing IMAGE_DEF into the
// two-block loop every pico-sdk flash image has (crt0.S defaults
// PICO_CRT0_INCLUDE_PICOBIN_END_BLOCK to 1).  The ROM walks the loop to
// validate the image; a lone block pointing at itself is legal in the
// header format but is not what any known-good RP2350 firmware ships.
//
// endOff is a free flash offset just past the kernel.  The ignored end
// block's LAST item carries a *negative* offset back to the IMAGE_DEF.
//
// This is the ARM layout: IMAGE_TYPE is the only item before LAST, so the
// next-block word sits 12 bytes into the block.  The RISC-V block also has
// an ENTRY_POINT item, which moves that word; emitRP2350RVUF2 locates it
// with rp2350NextBlockWord instead.
func rp2350CloseBlockLoop(flat []byte, markerOff, endOff uint32) {
	// IMAGE_DEF: LAST item's next-block word (marker + 12).
	binary.LittleEndian.PutUint32(flat[markerOff+12:markerOff+16], endOff-markerOff)
	rp2350WriteEndBlock(flat, markerOff, endOff)
}

// rp2350NextBlockWord walks the items of an IMAGE_DEF and returns the offset
// of the "next block" word (the one immediately after the 2BS_LAST item).
// Item size is in words for ordinary items; LAST is the only 2-byte-size
// form and is padded to a word boundary before the offset.
func rp2350NextBlockWord(flat []byte, markerOff, limit int) (int, error) {
	for pos := markerOff + 4; pos+4 <= limit && pos < markerOff+4096; {
		id := flat[pos]
		if id == 0xff { // PICOBIN_BLOCK_ITEM_2BS_LAST
			return pos + 4, nil
		}
		sizeWords := int(flat[pos+1])
		if sizeWords <= 0 {
			return 0, fmt.Errorf("PICOBIN item 0x%02x at +%d has zero size", id, pos)
		}
		pos += 4 * sizeWords
	}
	return 0, fmt.Errorf("no PICOBIN LAST item in the IMAGE_DEF")
}

// buildBoot2 loads the prebuilt 2nd-stage bootloader (board/rp2040/boot2.bin,
// the official Pico SDK W25Q080 boot2 assembled to a position-independent
// blob), pads it to 252 bytes and appends the CRC-32/MPEG-2 the boot ROM
// verifies, returning the final 256 bytes.
//
// Rebuilding the blob from source needs the full pico-sdk register headers, so
// we vendor the assembled artifact and only compute the CRC here. The stage-2
// enables XIP and vectors through the table at 0x10000100 (our kernel).
func (b *Builder) buildBoot2() ([]byte, error) {
	src := filepath.Join(b.Root, "board", "rp2040", "boot2.bin")
	data, err := os.ReadFile(src)
	if err != nil {
		return nil, fmt.Errorf("boot2 missing (%s); run a full build first", src)
	}
	if len(data) > 252 {
		return nil, fmt.Errorf("boot2 too large: %d bytes (max 252)", len(data))
	}
	b.logf("      boot2  board/rp2040/boot2.bin (%d bytes)\n", len(data))
	padded := make([]byte, 252)
	copy(padded, data)
	crc := crc32mpeg2(padded)
	out := append(padded, 0, 0, 0, 0)
	binary.LittleEndian.PutUint32(out[252:256], crc)
	return out, nil
}

// emitRP2040UF2 packages the kernel ELF, the littlefs image and the stage-2
// bootloader into a single RP2040 UF2 for the RPI-RP2 drive.
func (b *Builder) emitRP2040UF2(elfPath string) error {
	boot2, err := b.buildBoot2()
	if err != nil {
		return err
	}
	kernel, kernelOff, err := elfToFlashRaw(elfPath)
	if err != nil {
		return err
	}
	ramdiskPath := b.fsImage()
	ramdisk, err := os.ReadFile(ramdiskPath)
	if err != nil {
		return fmt.Errorf("littlefs image missing (%s); run a full build first", ramdiskPath)
	}

	const fsOff = rpLfsOffset
	if int(kernelOff)+len(kernel) > fsOff {
		return fmt.Errorf("kernel (%d bytes at flash 0x%x) overruns littlefs partition at flash 0x%x",
			len(kernel), rp2040FlashBase+uint32(kernelOff), rp2040FlashBase+fsOff)
	}
	if kernelOff < 256 {
		return fmt.Errorf("kernel starts at flash offset 0x%x, inside the 256-byte stage-2", kernelOff)
	}
	flat := make([]byte, fsOff+len(ramdisk))
	copy(flat[0:256], boot2)
	copy(flat[kernelOff:], kernel)
	copy(flat[fsOff:fsOff+len(ramdisk)], ramdisk)

	uf2 := flatToUF2(flat, rp2040UF2Family)
	out := filepath.Join(b.BuildDir, b.Target.Name+".uf2")
	if err := os.WriteFile(out, uf2, 0o644); err != nil {
		return err
	}
	b.logf("      %s (%d bytes, %d blocks)\n", relName(b.Root, out), len(uf2), len(uf2)/512)
	b.logf("      flash: hold BOOTSEL, drop %s onto RPI-RP2\n", relName(b.Root, out))
	b.logf("      littlefs partition at flash 0x%x (LBA %d)\n", rp2040FlashBase+fsOff, fsOff/512)
	return nil
}

// emitRP2350UF2 packages the RP2350 kernel and its littlefs partition into
// one UF2 for the "RP2350" BOOTSEL drive.
//
// There is no stage-2 on this chip: the boot ROM configures XIP itself, reads
// the ARM vector table at flash offset 0 and scans the first 4 KiB for the
// PICOBIN IMAGE_DEF block that image_def.S puts just after the vectors.
// So the kernel's raw flash image starts at byte 0, rather than at 0x100 as
// on the RP2040.  The littlefs image lives at 0x80000 (512 KiB).
func (b *Builder) emitRP2350UF2(elfPath string) error {
	kernel, koff, err := elfToFlashRaw(elfPath)
	if err != nil {
		return err
	}
	if koff != 0 {
		return fmt.Errorf("RP2350 kernel flash image starts at offset 0x%x, not at the start of flash", koff)
	}
	if len(kernel) < 0x100 {
		return fmt.Errorf("RP2350 kernel flash image is only %d bytes", len(kernel))
	}

	// Catch the two mistakes that a BOOTSEL drop reports only as "nothing
	// happened": a vector table that the ROM will not accept, and an
	// IMAGE_DEF that --gc-sections or a stray linker edit dropped.
	sp := binary.LittleEndian.Uint32(kernel[0:4])
	pc := binary.LittleEndian.Uint32(kernel[4:8])
	if sp < 0x20000000 || sp > 0x20082000 {
		return fmt.Errorf("RP2350 initial MSP 0x%08x is outside SRAM (0x20000000..0x20082000)", sp)
	}
	if pc < rp2040FlashBase || pc > rp2040FlashBase+0x1000000 || pc&1 == 0 {
		return fmt.Errorf("RP2350 reset vector 0x%08x is not a Thumb address in flash", pc)
	}
	marker := []byte{0xd3, 0xde, 0xff, 0xff}
	markerOff := -1
	for off := 0; off+4 <= len(kernel) && off < 4096; off += 4 {
		if kernel[off] == marker[0] && kernel[off+1] == marker[1] &&
			kernel[off+2] == marker[2] && kernel[off+3] == marker[3] {
			markerOff = off
			break
		}
	}
	if markerOff < 0 {
		return fmt.Errorf("RP2350 kernel has no PICOBIN IMAGE_DEF marker in the first 4 KiB (check KEEP(*(.picobin_block)) in kernel.ld)")
	}

	ramdiskPath := b.fsImage()
	ramdisk, err := os.ReadFile(ramdiskPath)
	if err != nil {
		return fmt.Errorf("littlefs image missing (%s); run a full build first", ramdiskPath)
	}

	const fsOff = rpLfsOffset
	if len(kernel) > fsOff {
		return fmt.Errorf("kernel (%d bytes) overruns littlefs partition at flash 0x%x",
			len(kernel), rp2040FlashBase+fsOff)
	}
	flat := make([]byte, fsOff+len(ramdisk))
	copy(flat, kernel)
	copy(flat[fsOff:], ramdisk)

	// Close the block loop like pico-sdk's crt0 does: an ignored metadata
	// block just past the kernel, with the IMAGE_DEF pointing forward to it
	// and it pointing back.  Everything between the kernel and the 512 KiB
	// littlefs partition is zero padding, so this costs no flash space.
	endOff := uint32((len(kernel) + 15) &^ 15)
	if endOff+20 > fsOff {
		return fmt.Errorf("RP2350 kernel leaves no room for the PICOBIN end block")
	}
	rp2350CloseBlockLoop(flat, uint32(markerOff), endOff)
	b.logf("      picobin: IMAGE_DEF @0x%x -> ignored end block @0x%x\n", markerOff, endOff)

	uf2 := append(rp2350AbsBlock(), flatToUF2(flat, rp2350UF2Family)...)
	out := filepath.Join(b.BuildDir, b.Target.Name+".uf2")
	if err := os.WriteFile(out, uf2, 0o644); err != nil {
		return err
	}
	b.logf("      %s (%d bytes, %d blocks)\n", relName(b.Root, out), len(uf2), len(uf2)/512)
	b.logf("      flash: hold BOOTSEL, drop %s onto the RP2350 drive\n", relName(b.Root, out))
	b.logf("      littlefs partition at flash 0x%x (LBA %d)\n", rp2040FlashBase+fsOff, fsOff/512)
	return nil
}

// emitRP2350RVUF2 packages the RISC-V RP2350 kernel and its littlefs
// partition for the same RP2350 BOOTSEL drive.
//
// The layout differs from the ARM image in exactly one place: there is no
// ARM vector table, so the IMAGE_DEF is the first thing in flash and its
// ENTRY_POINT item says where to jump (_start) and what stack to install.
// Everything downstream - the littlefs offset, the absolute block, the
// block-loop closing - is the same as the ARM packer, except for the family
// id and the next-block word's position in the block.
func (b *Builder) emitRP2350RVUF2(elfPath string) error {
	kernel, koff, err := elfToFlashRaw(elfPath)
	if err != nil {
		return err
	}
	if koff != 0 {
		return fmt.Errorf("RP2350-RV32 kernel flash image starts at offset 0x%x, not at the start of flash", koff)
	}
	if len(kernel) < 0x100 {
		return fmt.Errorf("RP2350-RV32 kernel flash image is only %d bytes", len(kernel))
	}

	marker := []byte{0xd3, 0xde, 0xff, 0xff}
	markerOff := -1
	for off := 0; off+4 <= len(kernel) && off < 4096; off += 4 {
		if kernel[off] == marker[0] && kernel[off+1] == marker[1] &&
			kernel[off+2] == marker[2] && kernel[off+3] == marker[3] {
			markerOff = off
			break
		}
	}
	if markerOff < 0 {
		return fmt.Errorf("RP2350-RV32 kernel has no PICOBIN IMAGE_DEF marker in the first 4 KiB (check KEEP(*(.picobin_block)) in kernel.ld)")
	}

	// The one mistake that a BOOTSEL drop reports only as "nothing
	// happened": a RISC-V block the ROM refuses because it is typed ARM or
	// has no entry point.  IMAGE_TYPE = EXE | CPU_RISCV | CHIP_RP2350.
	if markerOff+20 > len(kernel) || kernel[markerOff+4] != 0x42 ||
		binary.LittleEndian.Uint16(kernel[markerOff+6:markerOff+8]) != 0x1101 {
		return fmt.Errorf("RP2350-RV32 IMAGE_DEF at +%d is not typed EXE|RISCV|RP2350 (0x1101)", markerOff)
	}
	if kernel[markerOff+8] != 0x44 {
		return fmt.Errorf("RP2350-RV32 IMAGE_DEF has no ENTRY_POINT item; the ROM has nothing to jump to")
	}
	entry := binary.LittleEndian.Uint32(kernel[markerOff+12 : markerOff+16])
	sp := binary.LittleEndian.Uint32(kernel[markerOff+16 : markerOff+20])
	if entry < rp2040FlashBase || entry >= rp2040FlashBase+0x1000000 {
		return fmt.Errorf("RP2350-RV32 entry point 0x%08x is not in the XIP window", entry)
	}
	if sp < 0x20000000 || sp > 0x20082000 {
		return fmt.Errorf("RP2350-RV32 initial SP 0x%08x is outside SRAM (0x20000000..0x20082000)", sp)
	}

	nextWord, err := rp2350NextBlockWord(kernel, markerOff, len(kernel))
	if err != nil {
		return fmt.Errorf("RP2350-RV32 IMAGE_DEF: %w", err)
	}

	ramdiskPath := b.fsImage()
	ramdisk, err := os.ReadFile(ramdiskPath)
	if err != nil {
		return fmt.Errorf("littlefs image missing (%s); run a full build first", ramdiskPath)
	}

	const fsOff = rpLfsOffset
	if len(kernel) > fsOff {
		return fmt.Errorf("kernel (%d bytes) overruns littlefs partition at flash 0x%x",
			len(kernel), rp2040FlashBase+fsOff)
	}
	flat := make([]byte, fsOff+len(ramdisk))
	copy(flat, kernel)
	copy(flat[fsOff:], ramdisk)

	endOff := uint32((len(kernel) + 15) &^ 15)
	if endOff+20 > fsOff {
		return fmt.Errorf("RP2350-RV32 kernel leaves no room for the PICOBIN end block")
	}
	binary.LittleEndian.PutUint32(flat[nextWord:nextWord+4], endOff-uint32(markerOff))
	rp2350WriteEndBlock(flat, uint32(markerOff), endOff)
	b.logf("      picobin: IMAGE_DEF @0x%x -> ignored end block @0x%x (entry 0x%x, sp 0x%x)\n",
		markerOff, endOff, entry, sp)

	uf2 := append(rp2350AbsBlock(), flatToUF2(flat, rp2350RVUF2Family)...)
	out := filepath.Join(b.BuildDir, b.Target.Name+".uf2")
	if err := os.WriteFile(out, uf2, 0o644); err != nil {
		return err
	}
	b.logf("      %s (%d bytes, %d blocks)\n", relName(b.Root, out), len(uf2), len(uf2)/512)
	b.logf("      flash: hold BOOTSEL, drop %s onto the RP2350 drive\n", relName(b.Root, out))
	b.logf("      littlefs partition at flash 0x%x (LBA %d)\n", rp2040FlashBase+fsOff, fsOff/512)
	return nil
}

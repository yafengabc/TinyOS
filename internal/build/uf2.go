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
// there (XIP), and the littlefs image is flashed 1 MiB in.  Everything is
// wrapped in a UF2 (family 0xe48bff56) that drops onto the RPI-RP2 drive.

const rp2040UF2Family = 0xe48bff56
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

// elfToFlashRaw lays out the load segments of an RP2040 ELF as a raw binary
// addressed from rp2040FlashBase: byte N maps to flash address 0x10000000+N.
//
// Only segments that actually carry file bytes and whose physical address is
// inside the flash window belong in the image.  The .bss segment is NOLOAD
// (Filesz 0) and lld gives it Paddr == Vaddr in SRAM, so both checks skip it;
// the .data segment has Vaddr in SRAM but its Paddr is the flash LMA picked
// by the linker script's "AT > FLASH", which is what we flash.
func elfToFlashRaw(path string) ([]byte, error) {
	f, err := elf.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	const flashWindow = 0x1000000 // 16 MiB of XIP address space
	inFlash := func(p *elf.Prog) bool {
		return p.Type == elf.PT_LOAD && p.Filesz > 0 &&
			p.Paddr >= rp2040FlashBase && p.Paddr < rp2040FlashBase+flashWindow
	}
	var maxOff uint64
	for _, p := range f.Progs {
		if !inFlash(p) {
			continue
		}
		off := p.Paddr - rp2040FlashBase
		end := off + p.Filesz
		if end > maxOff {
			maxOff = end
		}
	}
	if maxOff == 0 {
		return nil, fmt.Errorf("elf %s has no load segments", path)
	}
	raw := make([]byte, maxOff)
	for _, p := range f.Progs {
		if !inFlash(p) {
			continue
		}
		off := p.Paddr - rp2040FlashBase
		data, err := io.ReadAll(p.Open())
		if err != nil {
			return nil, err
		}
		copy(raw[off:], data)
	}
	return raw, nil
}

// flatToUF2 converts a raw flash image into an RP2040-family UF2 stream.
func flatToUF2(flat []byte, family uint32) []byte {
	const blockPayload = 256
	numBlocks := (len(flat) + blockPayload - 1) / blockPayload
	out := make([]byte, 0, numBlocks*512)
	for i := 0; i < numBlocks; i++ {
		var buf [512]byte
		binary.LittleEndian.PutUint32(buf[0:4], 0x0A324655)   // magic_start0
		binary.LittleEndian.PutUint32(buf[4:8], 0x9E5D5157)   // magic_start1
		binary.LittleEndian.PutUint32(buf[8:12], 0x00002000)  // flags: FAMILY_ID_PRESENT
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
	kernel, err := elfToFlashRaw(elfPath)
	if err != nil {
		return err
	}
	ramdiskPath := filepath.Join(b.BuildDir, "ramdisk.bin")
	ramdisk, err := os.ReadFile(ramdiskPath)
	if err != nil {
		return fmt.Errorf("littlefs image missing (%s); run a full build first", ramdiskPath)
	}

	const kernelOff = 0x100
	const fsOff = 0x100000
	if kernelOff+len(kernel) > fsOff {
		return fmt.Errorf("kernel (%d bytes) overruns littlefs partition at flash 0x%x",
			len(kernel), rp2040FlashBase+fsOff)
	}
	flat := make([]byte, fsOff+len(ramdisk))
	copy(flat[0:256], boot2)
	copy(flat[kernelOff:kernelOff+len(kernel)], kernel)
	copy(flat[fsOff:fsOff+len(ramdisk)], ramdisk)

	uf2 := flatToUF2(flat, rp2040UF2Family)
	out := filepath.Join(b.BuildDir, "rp2040.uf2")
	if err := os.WriteFile(out, uf2, 0o644); err != nil {
		return err
	}
	b.logf("      %s (%d bytes, %d blocks)\n", relName(b.Root, out), len(uf2), len(uf2)/512)
	b.logf("      flash: hold BOOTSEL, drop %s onto RPI-RP2\n", relName(b.Root, out))
	b.logf("      littlefs partition at flash 0x%x (LBA %d)\n", rp2040FlashBase+fsOff, fsOff/512)
	return nil
}

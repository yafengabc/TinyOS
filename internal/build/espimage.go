package build

import (
	"crypto/sha256"
	"debug/elf"
	"encoding/binary"
	"fmt"
	"sort"
)

/* ESP32-C3 firmware image.
 *
 * The ROM bootloader reads this from SPI flash offset 0 (C3 differs from the
 * Xtensa parts, whose second stage lives at 0x1000), verifies the header and
 * checksum, copies every segment to its load address and jumps to the entry
 * point.  There is no filesystem, no partition table and no second stage: the
 * image is the kernel.
 *
 * Layout, all little-endian:
 *
 *   0   image header, 8 bytes   magic, segment count, flash mode, flash
 *                               size/frequency, entry point
 *   8   extended header, 16 B   WP pin, drive strength, chip id, chip
 *                               revisions, hash-appended flag
 *   24  segments                { load address, length, data } repeated
 *   ..  padding                 zeros, so that the checksum lands at a
 *                               16-byte boundary
 *   ..  checksum                1 byte, XOR of everything before it, seeded
 *                               with 0xEF
 *   ..  digest                  32 bytes SHA-256 of everything above, only
 *                               when the hash-appended flag is set
 */

const (
	ESPImageMagic = 0xE9
	ESPChipC3     = 5

	/* Byte 2: 0 = QIO, 1 = QOUT, 2 = DIO, 3 = DOUT.  DIO is what every
	   devkit flashes with, and it is the mode the ROM can always do. */
	ESPFlashDIO = 2

	/* Byte 3: high nibble = size (2 = 4 MiB), low nibble = frequency
	   (0 = 40 MHz). */
	ESPFlash4MB40MHz = 0x20

	espHeaderLen    = 24
	espSegHeaderLen = 8
	espChecksumSeed = 0xEF
	espMaxSegments  = 16
)

// ESPSegment is one contiguous run of bytes the ROM copies into RAM.
type ESPSegment struct {
	Addr uint32
	Data []byte
}

// ESPImage is everything the packer needs, with the flash parameters filled
// in by the caller so that a board can override them.
type ESPImage struct {
	Entry         uint32
	Segments      []ESPSegment
	FlashMode     byte
	FlashFreqSize byte
	ChipID        uint16
	MinRev        uint16
	MaxRev        uint16
	AppendSHA256  bool
}

// addrRange is a region the ROM is allowed to copy a segment into.  It exists
// to catch the expensive mistake of packaging a kernel that was linked for
// another board: a qemu-virt ELF at 0x80000000 would otherwise produce a
// perfectly well-formed image that the chip refuses to boot.
type addrRange struct {
	lo, hi uint32
	name   string
}

var espC3Loadable = []addrRange{
	{0x3FC80000, 0x3FCE0000, "DRAM"},       /* SRAM1, data bus          */
	{0x40380000, 0x403E0000, "IRAM"},       /* SRAM1, instruction bus   */
	{0x42000000, 0x42800000, "flash IROM"}, /* cached flash, read-only  */
	{0x3C000000, 0x3C800000, "flash DROM"}, /* cached flash, read-only  */
	{0x50000000, 0x50002000, "RTC memory"}, /* 8 KiB, survives reset    */
}

func (a addrRange) contains(addr, size uint32) bool {
	return addr >= a.lo && addr+size <= a.hi && addr+size >= addr
}

// BuildESPImage returns the bytes to write at flash offset 0.
func BuildESPImage(img ESPImage) ([]byte, error) {
	if len(img.Segments) == 0 {
		return nil, fmt.Errorf("esp image: no segments")
	}
	if len(img.Segments) > espMaxSegments {
		return nil, fmt.Errorf("esp image: %d segments, the ROM reads at most %d",
			len(img.Segments), espMaxSegments)
	}
	if img.FlashFreqSize == 0 {
		img.FlashFreqSize = ESPFlash4MB40MHz
	}
	if img.ChipID == 0 {
		img.ChipID = ESPChipC3
	}
	if img.MaxRev == 0 {
		img.MaxRev = 0xFFFF
	}
	if img.FlashMode > 3 {
		return nil, fmt.Errorf("esp image: flash mode %d is not one of 0..3", img.FlashMode)
	}

	segs := make([]ESPSegment, len(img.Segments))
	copy(segs, img.Segments)
	sort.Slice(segs, func(i, j int) bool { return segs[i].Addr < segs[j].Addr })

	for i, s := range segs {
		if len(s.Data) == 0 {
			return nil, fmt.Errorf("esp image: segment %d is empty", i)
		}
		if s.Addr%4 != 0 {
			return nil, fmt.Errorf("esp image: segment %d load address 0x%x is not 4-byte aligned",
				i, s.Addr)
		}
		if len(s.Data)%4 != 0 {
			return nil, fmt.Errorf("esp image: segment %d is %d bytes, not a multiple of 4",
				i, len(s.Data))
		}
		if i > 0 && s.Addr < segs[i-1].Addr+uint32(len(segs[i-1].Data)) {
			return nil, fmt.Errorf("esp image: segments at 0x%x and 0x%x overlap",
				segs[i-1].Addr, s.Addr)
		}
		ok := false
		for _, r := range espC3Loadable {
			if r.contains(s.Addr, uint32(len(s.Data))) {
				ok = true
				break
			}
		}
		if !ok {
			return nil, fmt.Errorf("esp image: segment %d (0x%x, %d bytes) is not in ESP32-C3 memory",
				i, s.Addr, len(s.Data))
		}
	}

	entryOK := false
	for _, s := range segs {
		if img.Entry >= s.Addr && img.Entry < s.Addr+uint32(len(s.Data)) {
			entryOK = true
			break
		}
	}
	if !entryOK {
		return nil, fmt.Errorf("esp image: entry 0x%x is not inside any segment", img.Entry)
	}

	out := make([]byte, espHeaderLen, espHeaderLen+len(segs)*(espSegHeaderLen+1024))
	out[0] = ESPImageMagic
	out[1] = byte(len(segs))
	out[2] = img.FlashMode
	out[3] = img.FlashFreqSize
	binary.LittleEndian.PutUint32(out[4:], img.Entry)
	out[8] = 0xEE /* WP pin disabled: the ROM must not touch it */
	binary.LittleEndian.PutUint16(out[12:], img.ChipID)
	binary.LittleEndian.PutUint16(out[15:], img.MinRev)
	binary.LittleEndian.PutUint16(out[17:], img.MaxRev)
	if img.AppendSHA256 {
		out[23] = 1
	}

	for _, s := range segs {
		var hdr [espSegHeaderLen]byte
		binary.LittleEndian.PutUint32(hdr[0:], s.Addr)
		binary.LittleEndian.PutUint32(hdr[4:], uint32(len(s.Data)))
		out = append(out, hdr[:]...)
		out = append(out, s.Data...)
	}

	/* The checksum byte is the last byte of a 16-byte block, so the padding
	   goes first.  Zeros do not change an XOR, which is why the order of
	   these two steps does not matter for the value - only for the length. */
	for (len(out)+1)%16 != 0 {
		out = append(out, 0)
	}
	out = append(out, espChecksum(out))

	if img.AppendSHA256 {
		sum := sha256.Sum256(out)
		out = append(out, sum[:]...)
	}
	return out, nil
}

func espChecksum(b []byte) byte {
	ck := byte(espChecksumSeed)
	for _, v := range b {
		ck ^= v
	}
	return ck
}

// ESPImageFromELF packages a linked kernel.
//
// Only file-backed PT_LOAD segments go in: .bss is not part of the image,
// exactly like the flat binary the PC target uses, and start.S zeroes it.
func ESPImageFromELF(path string, withSHA256 bool) ([]byte, []ESPSegment, error) {
	f, err := elf.Open(path)
	if err != nil {
		return nil, nil, err
	}
	defer f.Close()

	if f.Class != elf.ELFCLASS32 || f.Machine != elf.EM_RISCV {
		return nil, nil, fmt.Errorf("%s: not a RISC-V ELF32 file", path)
	}

	var segs []ESPSegment
	for _, p := range f.Progs {
		if p.Type != elf.PT_LOAD || p.Filesz == 0 {
			continue
		}
		if p.Filesz%4 != 0 || p.Vaddr%4 != 0 {
			return nil, nil, fmt.Errorf("%s: segment at 0x%x is not 4-byte aligned", path, p.Vaddr)
		}
		buf := make([]byte, p.Filesz)
		if _, err := p.ReadAt(buf, 0); err != nil {
			return nil, nil, fmt.Errorf("%s: reading segment at 0x%x: %w", path, p.Vaddr, err)
		}
		segs = append(segs, ESPSegment{Addr: uint32(p.Vaddr), Data: buf})
	}
	if len(segs) == 0 {
		return nil, nil, fmt.Errorf("%s: no loadable segments", path)
	}

	img, err := BuildESPImage(ESPImage{
		Entry:        uint32(f.Entry),
		Segments:     segs,
		FlashMode:    ESPFlashDIO,
		AppendSHA256: withSHA256,
	})
	if err != nil {
		return nil, nil, fmt.Errorf("%s: %w", path, err)
	}
	return img, segs, nil
}

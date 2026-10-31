package build

import (
	"debug/elf"
	"encoding/binary"
	"fmt"
	"os"
)

// NOT ADOPTED (2026-10-31): MiniELF is kept for reference only and is not
// part of the build pipeline.  It strips the ELF structure and the
// relocation tables, which kills binary distribution: the same image can
// not run at different APP_BASE values (QEMU vs. real boards).  User
// programs therefore stay standard ELF32 ET_DYN (PIE) + .rel/.rela +
// the LZ4TOS envelope; see kernel/elf.c.  Do not wire this format into
// the kernel or the toolchain.
//
// MiniELF is the on-disk container TinyOS uses for user programs.
//
// It replaces the ~1.5 KB of ELF headers (Ehdr + Phdrs + section header
// table) with a single 28 byte header, and carries the image LZ4-compressed.
// (The kernel-side loader kernel/minielf.c was never implemented.)
//
// Layout (little endian):
//
//	offset  size  field
//	0       4     magic "TOSX"
//	4       1     format version (1)
//	5       1     flags (bit0 = payload is LZ4 compressed)
//	6       2     reserved (0)
//	8       4     entry point (absolute virtual address)
//	12      4     lowest load address (virtual)
//	16      4     loaded size in memory (.bss included)
//	20      4     file-backed size (bytes present before compression)
//	24      4     payload size in this file
//	28      ...   payload
//
// Everything above the file-backed part of the image is .bss and is zeroed
// by the loader.
const (
	MiniMagic      = "TOSX"
	MiniVersion    = 1
	MiniHeaderSize = 28

	miniFlagCompressed = 1 << 0
)

// MiniImage is the result of converting an ELF.
type MiniImage struct {
	Data       []byte // the complete container
	Entry      uint32
	LoadAddr   uint32
	MemSize    uint32
	FileSize   uint32 // file-backed bytes before compression
	PayloadLen uint32
	Compressed bool
}

// ELFToMini converts a linked ELF32 executable into the TinyOS mini format.
//
// The ELF is expected to be a single contiguous load image (which is what
// user/user.ld produces): every PT_LOAD segment must be file-backed and
// ordered, and the gap between them is zero-filled.  The resulting image is
// [load, load+mem_size) with mem_size covering .bss as well.
func ELFToMini(path string, compress bool) (*MiniImage, error) {
	f, err := elf.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	if f.Class != elf.ELFCLASS32 || f.Machine != elf.EM_386 {
		return nil, fmt.Errorf("%s: not an i386 ELF32 file", path)
	}

	type seg struct {
		vaddr, filesz, memsz, off uint64
	}
	var segs []seg
	var load, end uint64 = ^uint64(0), 0

	for _, p := range f.Progs {
		if p.Type != elf.PT_LOAD || p.Memsz == 0 {
			continue
		}
		segs = append(segs, seg{vaddr: p.Vaddr, filesz: p.Filesz, memsz: p.Memsz, off: p.Off})
		if p.Vaddr < load {
			load = p.Vaddr
		}
		if p.Vaddr+p.Memsz > end {
			end = p.Vaddr + p.Memsz
		}
	}
	if len(segs) == 0 {
		return nil, fmt.Errorf("%s: no loadable segments", path)
	}
	if end <= load || end > 0xFFFFFFFF {
		return nil, fmt.Errorf("%s: bad load range", path)
	}
	if end-load > 1<<24 {
		return nil, fmt.Errorf("%s: image is %d bytes, too large for the mini format", path, end-load)
	}

	memSize := uint32(end - load)

	// Lay the file-backed bytes out at their virtual addresses; gaps (and
	// the .bss tail) stay zero.
	img := make([]byte, memSize)
	var fileEnd uint64
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	for _, s := range segs {
		if s.filesz == 0 {
			continue
		}
		if s.off+s.filesz > uint64(len(raw)) {
			return nil, fmt.Errorf("%s: segment past end of file", path)
		}
		copy(img[s.vaddr-load:], raw[s.off:s.off+s.filesz])
		if s.vaddr+s.filesz > fileEnd {
			fileEnd = s.vaddr + s.filesz
		}
	}

	fileSize := uint32(fileEnd - load)

	payload := img[:fileSize]
	flags := byte(0)
	if compress {
		bound := lz4CompressBound(len(payload))
		comp := make([]byte, bound)
		n := lz4CompressBlock(payload, comp)
		if n > 0 && n < len(payload) {
			payload = comp[:n]
			flags |= miniFlagCompressed
		}
	}

	out := make([]byte, MiniHeaderSize+len(payload))
	copy(out[0:4], MiniMagic)
	out[4] = MiniVersion
	out[5] = flags
	binary.LittleEndian.PutUint32(out[8:], uint32(f.Entry))
	binary.LittleEndian.PutUint32(out[12:], uint32(load))
	binary.LittleEndian.PutUint32(out[16:], memSize)
	binary.LittleEndian.PutUint32(out[20:], fileSize)
	binary.LittleEndian.PutUint32(out[24:], uint32(len(payload)))
	copy(out[MiniHeaderSize:], payload)

	return &MiniImage{
		Data:       out,
		Entry:      uint32(f.Entry),
		LoadAddr:   uint32(load),
		MemSize:    memSize,
		FileSize:   fileSize,
		PayloadLen: uint32(len(payload)),
		Compressed: flags&miniFlagCompressed != 0,
	}, nil
}

// DecodeMiniPayload returns the file-backed image bytes held by the
// container (decompressing when needed).
func DecodeMiniPayload(img *MiniImage) ([]byte, error) {
	payload := img.Data[MiniHeaderSize:]
	if img.Compressed {
		return lz4DecompressBlock(payload, int(img.FileSize))
	}
	if uint32(len(payload)) != img.FileSize {
		return nil, fmt.Errorf("stored payload is %d bytes, header says %d",
			len(payload), img.FileSize)
	}
	return payload, nil
}

// VerifyMini decodes the payload and checks it yields exactly FileSize bytes.
// It returns the decoded length.
func VerifyMini(img *MiniImage) (int, error) {
	out, err := DecodeMiniPayload(img)
	if err != nil {
		return 0, err
	}
	if uint32(len(out)) != img.FileSize {
		return 0, fmt.Errorf("decoded %d bytes, header says %d", len(out), img.FileSize)
	}
	return len(out), nil
}

// ParseMini decodes the header, for tests and the verifier.
func ParseMini(data []byte) (*MiniImage, error) {
	if len(data) < MiniHeaderSize {
		return nil, fmt.Errorf("truncated mini header")
	}
	if string(data[0:4]) != MiniMagic {
		return nil, fmt.Errorf("bad magic")
	}
	if data[4] != MiniVersion {
		return nil, fmt.Errorf("unsupported version %d", data[4])
	}
	payloadLen := binary.LittleEndian.Uint32(data[24:])
	if uint64(MiniHeaderSize)+uint64(payloadLen) != uint64(len(data)) {
		return nil, fmt.Errorf("payload length %d does not match file size %d", payloadLen, len(data))
	}
	return &MiniImage{
		Data:       data,
		Entry:      binary.LittleEndian.Uint32(data[8:]),
		LoadAddr:   binary.LittleEndian.Uint32(data[12:]),
		MemSize:    binary.LittleEndian.Uint32(data[16:]),
		FileSize:   binary.LittleEndian.Uint32(data[20:]),
		PayloadLen: payloadLen,
		Compressed: data[5]&miniFlagCompressed != 0,
	}, nil
}
